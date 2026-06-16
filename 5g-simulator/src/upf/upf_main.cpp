// ============================================================
// UPF — User Plane Function (TS 23.501 §6.2.3)
//
// REAL ROLE:
//   The UPF is the 5G counterpart of the 4G P-GW's user plane.
//   After CUPS (Control/User Plane Separation), the UPF handles:
//     - GTP-U packet forwarding (from gNB to Data Network and back)
//     - PDR/FAR enforcement (instructed by SMF via N4/PFCP)
//     - QoS enforcement, traffic accounting
//     - ULCL (Uplink Classifier) for edge computing scenarios
//
// THIS SIM:
//   - PFCP UDP server on :8805 (N4 interface to SMF)
//   - Handles PFCP Session Establishment Request → Response
//   - Programs PDR/FAR rules (logged only, no real packet forwarding)
//   - Registers with NRF as UPF
//   - Writes PFCP frames to pcap (Wireshark: decode-as PFCP on UDP/8805)
//
// PFCP Session Establishment flow (TS 29.244 §7.5.2):
//   SMF → UPF: Session Establishment Request
//     - Node-ID (SMF's IP)
//     - CP F-SEID (SMF's SEID for this session)
//     - Create PDR: match rule for this UE's traffic
//     - Create FAR: action (forward to DN or gNB)
//   UPF → SMF: Session Establishment Response
//     - Cause: Request Accepted (1)
//     - Node-ID (UPF's IP)
//     - UP F-SEID (UPF's SEID for this session, used in future N4 messages)
//
// INTERVIEW Q: "How does the UPF know where to send packets?"
// ANSWER: "SMF installs forwarding rules via PFCP:
//   - UL PDR: match TEID from gNB → FAR: forward to Data Network (internet)
//   - DL PDR: match UE IP from DN → FAR: encapsulate in GTP-U → forward to gNB
//   The gNB's GTP-U address and TEID are sent by the AMF/SMF when the PDU
//   session is established (in the NGAP PDU Session Resource Setup message)."
//
// INTERVIEW Q: "What is the N6 interface?"
// ANSWER: "N6 = UPF to Data Network (internet/enterprise). Just regular IP routing.
//   N3 = UPF to gNB (GTP-U tunnels). N4 = UPF to SMF (PFCP control)."
// ============================================================
#include "common/socket_wrapper.h"
#include "common/pcap_writer.h"
#include "common/logger.h"
#include "common/nrf_client.h"
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/socket.h>

using Logger::Level;

static PcapWriter& pcap() { return PcapWriter::instance(); }

// ── PFCP parser — minimal, handles Session Est Req ───────────
namespace pfcp {

static uint16_t get16(const uint8_t* b) { return uint16_t(b[0]<<8)|b[1]; }
static uint32_t get32(const uint8_t* b) {
    return uint32_t(b[0]<<24)|uint32_t(b[1]<<16)|uint32_t(b[2]<<8)|b[3];
}

struct Header {
    uint8_t  version_flags;
    uint8_t  msg_type;
    uint16_t msg_len;
    uint64_t seid;       // valid if S-flag set
    uint32_t seq;
    bool     has_seid;
};

static bool parseHeader(const std::vector<uint8_t>& pkt, Header& h) {
    if (pkt.size() < 4) return false;
    h.version_flags = pkt[0];
    h.msg_type      = pkt[1];
    h.msg_len       = get16(pkt.data() + 2);
    h.has_seid      = (h.version_flags & 0x01) != 0;
    size_t off = 4;
    if (h.has_seid) {
        if (pkt.size() < off + 8 + 4) return false;
        h.seid = 0;
        for (int i = 0; i < 8; ++i) { h.seid = (h.seid << 8) | pkt[off+i]; }
        off += 8;
    }
    if (pkt.size() < off + 3) return false;
    h.seq = (uint32_t(pkt[off])<<16) | (uint32_t(pkt[off+1])<<8) | pkt[off+2];
    return true;
}

// Build PFCP Session Establishment Response (type=51)
static std::vector<uint8_t> buildRsp(uint32_t upf_ip, uint64_t up_seid, uint32_t seq) {
    auto put16 = [](std::vector<uint8_t>& v, uint16_t x){
        v.push_back(uint8_t(x>>8)); v.push_back(uint8_t(x)); };
    auto ie = [&](std::vector<uint8_t>& out, uint16_t t, const std::vector<uint8_t>& val){
        put16(out,t); put16(out, uint16_t(val.size()));
        out.insert(out.end(), val.begin(), val.end()); };

    std::vector<uint8_t> ies;
    // Cause=1 (Request Accepted)
    ie(ies, 19, {0x01});
    // Node-ID: IPv4 UPF
    std::vector<uint8_t> nid = {0x00,
        uint8_t(upf_ip>>24),uint8_t(upf_ip>>16),uint8_t(upf_ip>>8),uint8_t(upf_ip)};
    ie(ies, 60, nid);
    // F-SEID: UP F-SEID
    std::vector<uint8_t> fseid = {0x02}; // IPv4 present
    for (int i = 56; i >= 0; i -= 8) fseid.push_back(uint8_t(up_seid >> i));
    fseid.push_back(uint8_t(upf_ip>>24)); fseid.push_back(uint8_t(upf_ip>>16));
    fseid.push_back(uint8_t(upf_ip>>8));  fseid.push_back(uint8_t(upf_ip));
    ie(ies, 57, fseid);

    std::vector<uint8_t> body;
    for (int i = 0; i < 8; ++i) body.push_back(0); // echo back SEID = 0 for now
    body.push_back(uint8_t(seq>>16)); body.push_back(uint8_t(seq>>8)); body.push_back(uint8_t(seq));
    body.push_back(0);
    body.insert(body.end(), ies.begin(), ies.end());

    uint16_t msg_len = uint16_t(body.size());
    std::vector<uint8_t> pkt = {0x21, 51, uint8_t(msg_len>>8), uint8_t(msg_len)};
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

} // namespace pfcp

// ── UPF session table ─────────────────────────────────────────
struct PfcpSession {
    uint64_t cp_seid;
    uint64_t up_seid;
    uint32_t pdr_count{0};
    uint32_t far_count{0};
};

static std::vector<PfcpSession> s_sessions;
static std::atomic<uint64_t>    s_up_seid_counter{0xABC0000000000001ULL};

int main() {
    Logger::setSessionFile("g5_upf_session.log");
    Logger::setLevelFromEnv();

    const uint16_t PFCP_PORT = 8805;
    pcap().open("5g_upf_capture.pcap");

    Logger::step("UPF starting");
    Logger::sys("UPF: User Plane Function (TS 23.501 §6.2.3)");
    Logger::sys("UPF: N4 (PFCP) on UDP :8805 | N3 (GTP-U) conceptual | N6 (to internet) conceptual");
    Logger::sys("UPF: Wireshark: decode-as PFCP on UDP/8805 to see PDRs and FARs");

    const char* UPF_SELF_HOST = std::getenv("UPF_HOST") ? std::getenv("UPF_HOST") : "127.0.0.1";
    nrfclient::registerSelf(Logger::CLR_UPF, " UPF  ", "upf-1", "UPF", UPF_SELF_HOST, PFCP_PORT);

    // Bind UDP socket for PFCP
    int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { Logger::warn(" UPF  ", "socket() failed"); return 1; }
    int opt = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(PFCP_PORT);

    if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::warn(" UPF  ", "bind() on UDP :8805 failed — is another UPF running?");
        ::close(fd); return 1;
    }
    Logger::sys("UPF: PFCP UDP server bound on :" + std::to_string(PFCP_PORT));
    Logger::sys("UPF: waiting for N4 PFCP Session Establishment from SMF...");

    while (true) {
        uint8_t buf[4096] = {};
        sockaddr_in peer{};
        socklen_t plen = sizeof(peer);
        ssize_t n = ::recvfrom(fd, buf, sizeof(buf), 0, (sockaddr*)&peer, &plen);
        if (n <= 0) break;

        std::vector<uint8_t> pkt(buf, buf + n);

        pfcp::Header h;
        if (!pfcp::parseHeader(pkt, h)) {
            Logger::warn(" UPF  ", "malformed PFCP header");
            continue;
        }

        Logger::upf(Level::ENGINEER,
            "[PFCP] ← msg_type=" + std::to_string(h.msg_type) +
            " seq=" + std::to_string(h.seq) + " seid=" + std::to_string(h.seid));

        if (h.msg_type == 50) { // Session Establishment Request
            Logger::upf(Level::BEGINNER,
                "UPF <- SMF: PFCP Session Establishment Request [TS 29.244 §7.5.2]");
            Logger::ie_field("  N4 session being created for a new UE PDU session");
            Logger::ie_field("  UPF will program dataplane with PDRs (match rules) and FARs (actions)");

            Logger::upf(Level::INTERVIEW_T,
                "[INTERVIEW] Q: How does UPF know where to send downlink packets?");
            Logger::upf(Level::INTERVIEW_C,
                "A: SMF sends a Create FAR with 'Forwarding Parameters' that includes");
            Logger::upf(Level::INTERVIEW_C,
                "   the destination interface (N3=toward gNB) and outer header creation");
            Logger::upf(Level::INTERVIEW_C,
                "   (GTP-U header with the gNB's TEID). The gNB TEID comes from the");
            Logger::upf(Level::INTERVIEW_C,
                "   NGAP PDU Session Resource Setup Response from the gNB to AMF.");

            uint64_t up_seid = s_up_seid_counter.fetch_add(1);
            s_sessions.push_back({h.seid, up_seid, 1, 1}); // 1 PDR, 1 FAR

            Logger::upf(Level::ENGINEER,
                "[PFCP] Programmed: PDR-ID=1 (match access-side traffic) + FAR-ID=1 (forward)");
            Logger::upf(Level::ENGINEER,
                "[PFCP] UP F-SEID=" + [&]{
                    char b[32]; std::snprintf(b,sizeof(b),"0x%016llX",
                        (unsigned long long)up_seid); return std::string(b); }());
            Logger::sys("UPF: N4 session #" + std::to_string(s_sessions.size()) +
                " established  CP-SEID=" + std::to_string(h.seid) +
                " UP-SEID=" + std::to_string(up_seid));

            auto rsp = pfcp::buildRsp(PcapWriter::IP_UPF, up_seid, h.seq);

            // PCAP: Request arrived (SMF→UPF), Response leaving (UPF→SMF)
            pcap().writeUdp(pkt, PcapWriter::IP_SMF, PcapWriter::PORT_PFCP,
                                  PcapWriter::IP_UPF, PcapWriter::PORT_PFCP);
            pcap().writeUdp(rsp,  PcapWriter::IP_UPF, PcapWriter::PORT_PFCP,
                                  PcapWriter::IP_SMF, PcapWriter::PORT_PFCP);

            ::sendto(fd, rsp.data(), rsp.size(), 0, (sockaddr*)&peer, plen);

            Logger::upf(Level::BEGINNER,
                "UPF -> SMF: PFCP Session Establishment Response — cause=Request Accepted ✓");

        } else if (h.msg_type == 52) { // Session Modification Request
            Logger::upf(Level::BEGINNER,
                "UPF <- SMF: PFCP Session Modification Request (path switch after HO)");
            Logger::ie_field("  Update FAR: new gNB TEID after handover");

            std::vector<uint8_t> rsp = pfcp::buildRsp(PcapWriter::IP_UPF, 0, h.seq);
            rsp[1] = 53; // Session Modification Response
            ::sendto(fd, rsp.data(), rsp.size(), 0, (sockaddr*)&peer, plen);
            Logger::upf(Level::BEGINNER, "UPF -> SMF: PFCP Session Modification Response ✓");

        } else if (h.msg_type == 54) { // Session Deletion Request
            Logger::upf(Level::BEGINNER, "UPF <- SMF: PFCP Session Deletion Request");
            std::vector<uint8_t> rsp = pfcp::buildRsp(PcapWriter::IP_UPF, 0, h.seq);
            rsp[1] = 55; // Session Deletion Response
            ::sendto(fd, rsp.data(), rsp.size(), 0, (sockaddr*)&peer, plen);
            Logger::upf(Level::BEGINNER, "UPF -> SMF: PFCP Session Deletion Response ✓");

        } else {
            Logger::warn(" UPF  ", "unhandled PFCP msg_type=" + std::to_string(h.msg_type));
        }
    }

    ::close(fd);
    Logger::shutdown();
}
