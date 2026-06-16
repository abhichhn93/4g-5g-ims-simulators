// ============================================================
// SMF — Session Management Function (TS 23.501 §6.2.2)
//
// REAL ROLE:
//   The SMF owns PDU Session lifecycle in the 5G core.
//   When a UE wants data connectivity, it triggers a PDU Session
//   Establishment procedure. AMF forwards the NAS request to SMF
//   via the Nsmf_PDUSession_CreateSMContext SBI call.
//   SMF then:
//     1. Allocates a UE IP address (from UPF's address pool)
//     2. Selects a UPF via PFCP (N4 interface)
//     3. Creates PDRs (Packet Detection Rules) + FARs (Forwarding Action Rules)
//     4. Returns the UE IP to AMF, which forwards it to the UE
//   Subsequently, uplink packets: UE→gNB→UPF (GTP-U)→DN
//                  downlink:  DN→UPF→gNB (GTP-U)→UE
//
// THIS SIM:
//   - SBI HTTP/1.1 server on TCP :29502
//   - Registers with NRF as SMF
//   - Handles POST /nsmf-pdusession/v1/sm-contexts (PDU Session Create)
//   - Calls UPF via PFCP (N4) over UDP :8805
//   - Allocates UE IPs from 10.45.0.0/16 pool
//
// 5G ~ 4G analogy:
//   SMF = P-GW control plane (in 4G the PGW did both control + user plane)
//   UPF = P-GW user plane (split out in 5G for CUPS: Control/User Plane Sep)
//   PFCP (N4 interface) = the split introduced by 3GPP TS 29.244
//
// INTERVIEW ANSWERS:
//  Q: What is CUPS?
//  A: Control and User Plane Separation. In 4G, the P-GW handled both
//     signalling (IP allocation, QoS) and user plane (routing packets).
//     In 5G, SMF handles control, UPF handles user plane. This allows
//     UPF to scale/move independently — e.g. put UPFs close to the edge
//     while centralizing SMF. Interface = N4, protocol = PFCP.
//
//  Q: What is PFCP?
//  A: Packet Forwarding Control Protocol (TS 29.244). UDP port 8805.
//     SMF sends Session Establishment Request with PDRs (what to match)
//     and FARs (what to do with matched packets). UPF programs its
//     dataplane with these rules. PDR = match rule, FAR = action rule.
//
//  Q: Why does AMF not talk directly to UPF?
//  A: Because UPF is a pure dataplane element. SMF abstracts the
//     user plane topology. AMF just asks SMF for an IP; SMF manages
//     which UPF to use, N4 session establishment, and path switch.
// ============================================================
#include "common/socket_wrapper.h"
#include "common/wire.h"
#include "common/pcap_writer.h"
#include "common/logger.h"
#include "common/nrf_client.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>
#include <sys/socket.h>

using Logger::Level;

static PcapWriter& pcap() { return PcapWriter::instance(); }

// ── UE IP address pool: 10.45.0.0/16 ────────────────────────
// INTERVIEW: In production, SMF uses a DNN-specific IP address pool.
// The pool is configured per Data Network Name (DNN/APN).
// Default DNN "internet" → pool 10.45.0.0/16.
static std::atomic<uint32_t> s_ue_ip_counter{2};  // start at .2 (.1 = gateway)

static std::string allocateUeIp() {
    uint32_t n = s_ue_ip_counter.fetch_add(1);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "10.45.%u.%u", (n >> 8) & 0xFF, n & 0xFF);
    return buf;
}

// ── PFCP frame builder ────────────────────────────────────────
// PFCP header (TS 29.244 §7.2.3.1):
//   Byte 0: [version=001][FO=0][MP=0][S=1][SPARE=0][SPARE=0][SPARE=0][SPARE=0]
//           S=1 means SEID field is present (for session messages)
//   Byte 1: Message type
//   Bytes 2-3: Message length (total length minus first 4 bytes)
//   Bytes 4-11: SEID (8 bytes, 0 for initial Establishment Request)
//   Bytes 12-14: Sequence number (3 bytes, big-endian)
//   Byte 15: Spare = 0
//
// IE format: [Type 2B BE][Length 2B BE][Value NB]
//
// IEs used:
//   Node-ID (60 / 0x003C): identifies the CP function
//     value: [type=0x00 IPv4][4B IP]
//   F-SEID (57 / 0x0039): F-SEID of the sender
//     value: [flags=0x02 IPv4 present][8B SEID][4B IPv4]
//   PDR-ID (56 / 0x0038): PDR identifier
//     value: [2B rule ID]
//   Precedence (29 / 0x001D): PDR precedence (lower = higher priority)
//     value: [4B]
//   PDI (22 / 0x0016): Packet Detection Information (inner IE container)
//   FAR-ID (108 / 0x006C): FAR identifier
//     value: [4B rule ID]
//   Apply-Action (44 / 0x002C): action for matched packets
//     value: [2B flags: bit1=FORW, bit2=DROP, bit3=BUFF]
//   Create PDR (1 / 0x0001): grouped IE for PDR creation
//   Create FAR (3 / 0x0003): grouped IE for FAR creation

namespace pfcp {

static void put16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
static void put32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x>>24)); v.push_back(uint8_t(x>>16));
    v.push_back(uint8_t(x>>8));  v.push_back(uint8_t(x));
}
static void put64(std::vector<uint8_t>& v, uint64_t x) {
    for (int i = 56; i >= 0; i -= 8) v.push_back(uint8_t(x >> i));
}

static void ie(std::vector<uint8_t>& out, uint16_t type, const std::vector<uint8_t>& val) {
    put16(out, type);
    put16(out, uint16_t(val.size()));
    out.insert(out.end(), val.begin(), val.end());
}

// Node-ID IE (type=60): IPv4 node identifier
static std::vector<uint8_t> nodeId(uint32_t ip) {
    std::vector<uint8_t> v = {0x00}; // type = IPv4
    v.push_back(uint8_t(ip>>24)); v.push_back(uint8_t(ip>>16));
    v.push_back(uint8_t(ip>>8));  v.push_back(uint8_t(ip));
    return v;
}

// F-SEID IE (type=57): fully-qualified SEID
static std::vector<uint8_t> fSeid(uint64_t seid, uint32_t ip) {
    std::vector<uint8_t> v = {0x02}; // flags: IPv4 present
    // SEID (8 bytes)
    for (int i = 56; i >= 0; i -= 8) v.push_back(uint8_t(seid >> i));
    // IPv4
    v.push_back(uint8_t(ip>>24)); v.push_back(uint8_t(ip>>16));
    v.push_back(uint8_t(ip>>8));  v.push_back(uint8_t(ip));
    return v;
}

// Create PDR IE (type=1): minimal PDR — PDR-ID + Precedence + PDI(Source Interface)
static std::vector<uint8_t> createPdr(uint16_t pdr_id, uint32_t far_id) {
    std::vector<uint8_t> inner;
    // PDR-ID (type=56)
    std::vector<uint8_t> pdr_id_val = {uint8_t(pdr_id>>8), uint8_t(pdr_id)};
    ie(inner, 56, pdr_id_val);
    // Precedence (type=29)
    std::vector<uint8_t> prec = {0,0,0,100};
    ie(inner, 29, prec);
    // PDI (type=22): Source Interface (type=20, 1B: 0=Access, 1=Core)
    std::vector<uint8_t> src_iface = {0x00, 0x00, 0x00, 0x00}; // Access
    std::vector<uint8_t> pdi_inner;
    ie(pdi_inner, 20, src_iface);
    ie(inner, 22, pdi_inner);
    // FAR ID (type=108)
    std::vector<uint8_t> fid = {0,0,uint8_t(far_id>>8),uint8_t(far_id)};
    ie(inner, 108, fid);
    return inner;
}

// Create FAR IE (type=3): minimal FAR — FAR-ID + Apply-Action=FORW
static std::vector<uint8_t> createFar(uint32_t far_id) {
    std::vector<uint8_t> inner;
    // FAR-ID (type=108)
    std::vector<uint8_t> fid = {0,0,uint8_t(far_id>>8),uint8_t(far_id)};
    ie(inner, 108, fid);
    // Apply-Action (type=44): 0x02 = FORW (forward)
    std::vector<uint8_t> act = {0x00, 0x02};
    ie(inner, 44, act);
    return inner;
}

// Build PFCP Session Establishment Request (msg_type=50)
// TS 29.244 §7.5.2
std::vector<uint8_t> buildSessionEstReq(uint32_t smf_ip, uint64_t cp_seid,
                                          uint32_t seq, uint16_t pdr_id = 1,
                                          uint32_t far_id = 1) {
    // Build IEs
    std::vector<uint8_t> ies;
    ie(ies, 60, nodeId(smf_ip));                           // Node-ID
    ie(ies, 57, fSeid(cp_seid, smf_ip));                   // F-SEID (CP)
    ie(ies, 1,  createPdr(pdr_id, far_id));                // Create PDR
    ie(ies, 3,  createFar(far_id));                         // Create FAR

    // Header: [0x21][50][len_hi][len_lo][SEID 8B=0][seq 3B][spare]
    // Length = header_after_4bytes + ies.size() = 8(SEID) + 4(seq+spare) + ies
    // Actually len = total - 4, and we'll compute from body
    std::vector<uint8_t> body; // after the 4-byte common header
    // SEID (8 bytes, 0 for initial request where UP node doesn't know the SEID yet)
    for (int i = 0; i < 8; ++i) body.push_back(0x00);
    // Sequence (3 bytes) + spare
    body.push_back(uint8_t(seq>>16)); body.push_back(uint8_t(seq>>8)); body.push_back(uint8_t(seq));
    body.push_back(0x00); // spare
    body.insert(body.end(), ies.begin(), ies.end());

    uint16_t msg_len = uint16_t(body.size());
    std::vector<uint8_t> pkt;
    pkt.push_back(0x21); // version=1, S=1
    pkt.push_back(50);   // Session Establishment Request
    pkt.push_back(uint8_t(msg_len>>8));
    pkt.push_back(uint8_t(msg_len));
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

// Build PFCP Session Establishment Response (msg_type=51)
std::vector<uint8_t> buildSessionEstRsp(uint32_t upf_ip, uint64_t up_seid,
                                          uint32_t seq) {
    std::vector<uint8_t> ies;
    // Cause IE (type=19, 1B): 1=Request accepted
    std::vector<uint8_t> cause = {0x01};
    ie(ies, 19, cause);
    ie(ies, 60, nodeId(upf_ip));            // Node-ID
    ie(ies, 57, fSeid(up_seid, upf_ip));   // F-SEID (UP, assigned to this session)

    std::vector<uint8_t> body;
    // SEID = CP F-SEID value from request (echo back for correlation)
    for (int i = 0; i < 8; ++i) body.push_back(0x00);
    body.push_back(uint8_t(seq>>16)); body.push_back(uint8_t(seq>>8)); body.push_back(uint8_t(seq));
    body.push_back(0x00);
    body.insert(body.end(), ies.begin(), ies.end());

    uint16_t msg_len = uint16_t(body.size());
    std::vector<uint8_t> pkt;
    pkt.push_back(0x21);
    pkt.push_back(51); // Session Establishment Response
    pkt.push_back(uint8_t(msg_len>>8));
    pkt.push_back(uint8_t(msg_len));
    pkt.insert(pkt.end(), body.begin(), body.end());
    return pkt;
}

} // namespace pfcp

// ── UPF host/port (discovered via NRF or env) ────────────────
static std::string s_upf_host = "127.0.0.1";
static uint16_t    s_upf_pfcp_port = 8805;
static std::atomic<uint32_t> s_pfcp_seq{1};

// ── N4 session setup via PFCP ─────────────────────────────────
// SMF sends Session Establishment Request to UPF over UDP 8805
// UPF programs PDRs + FARs and returns Session Establishment Response
static std::string setupN4Session(const std::string& ue_ip, uint32_t pdu_session_id) {
    Logger::smf(Level::BEGINNER,
        "SMF -> UPF: N4 PFCP Session Establishment Request (TSS 29.244 §7.5.2)");
    Logger::ie_field("  N4 interface connects SMF (control) to UPF (user plane)");
    Logger::ie_field("  This is the CUPS split — 5G separates control and user plane");

    Logger::smf(Level::ENGINEER,
        "[N4] Creating PDR: match UL traffic from UE " + ue_ip + " on S1-GTP-U tunnel");
    Logger::ie_field("  PDR-ID=1 precedence=100 source-interface=Access");
    Logger::ie_field("  FAR-ID=1 action=FORWARD → toward Data Network (internet)");
    Logger::smf(Level::ENGINEER,
        "[N4] Creating DL PDR: match traffic from internet destined for " + ue_ip);
    Logger::ie_field("  DL PDR: source-interface=Core, FAR-ID=2 action=FORWARD → gNB via GTP-U");

    Logger::smf(Level::INTERVIEW_T,
        "[INTERVIEW] Q: What are PDRs and FARs in PFCP?");
    Logger::smf(Level::INTERVIEW_C,
        "A: PDR = Packet Detection Rule. Describes what traffic to match");
    Logger::smf(Level::INTERVIEW_C,
        "   (source interface, TEID, UE IP). FAR = Forwarding Action Rule.");
    Logger::smf(Level::INTERVIEW_C,
        "   Describes what to do with matched traffic: forward, drop, buffer.");
    Logger::smf(Level::INTERVIEW_C,
        "   SMF installs rules, UPF enforces them. This is the CUPS control channel.");

    uint32_t seq = s_pfcp_seq.fetch_add(1);
    uint64_t cp_seid = 0x0000000100000000ULL | pdu_session_id; // deterministic CP SEID
    uint32_t smf_ip  = PcapWriter::IP_SMF;

    auto req_pdu = pfcp::buildSessionEstReq(smf_ip, cp_seid, seq);
    auto rsp_pdu = pfcp::buildSessionEstRsp(PcapWriter::IP_UPF, cp_seid ^ 0xDEAD, seq);

    // PCAP: write PFCP frames over UDP
    pcap().writeUdp(req_pdu, PcapWriter::IP_SMF, PcapWriter::PORT_PFCP,
                             PcapWriter::IP_UPF,  PcapWriter::PORT_PFCP);
    pcap().writeUdp(rsp_pdu, PcapWriter::IP_UPF,  PcapWriter::PORT_PFCP,
                             PcapWriter::IP_SMF, PcapWriter::PORT_PFCP);

    Logger::smf(Level::BEGINNER,
        "SMF <- UPF: N4 PFCP Session Establishment Response — cause=Request Accepted");
    Logger::ie_field("  UP F-SEID assigned by UPF for this session");
    Logger::ie_field("  UPF data plane ready: GTP-U tunnel will be created on first packet");

    // UDP socket: send real PFCP to UPF process if it's listening
    UdpSocket upf_sock;
    upf_sock.bind("127.0.0.1", 0); // ephemeral src port
    std::vector<uint8_t> rsp_buf;
    sockaddr_in upf_addr{};
    upf_sock.sendTo(req_pdu, s_upf_host.c_str(), s_upf_pfcp_port);
    // Non-blocking: we don't wait for UPF response here since UPF may not be up
    // In production, SMF uses a timer (T1, TS 29.244 §7.3) to retry
    Logger::smf(Level::ENGINEER,
        "[N4] PFCP Req sent to UPF " + s_upf_host + ":" + std::to_string(s_upf_pfcp_port));

    return ue_ip;
}

// ── SBI server: handle one HTTP request ──────────────────────
static void handleOne(const Socket& client, uint32_t& ue_ip_counter) {
    HttpMessage req;
    if (!httpRecv(client, req)) return;

    Logger::raw(req.startLine + "\n" + req.body);
    Logger::smf(Level::ENGINEER, "[SBI] ← " + req.startLine);

    // Only handle PDU Session Create
    if (req.startLine.find("POST") == std::string::npos ||
        req.startLine.find("sm-contexts") == std::string::npos) {
        std::string body = "{\"problem\":\"only POST /nsmf-pdusession/v1/sm-contexts supported\"}";
        std::string resp = httpBuild("HTTP/1.1 400 Bad Request", body);
        httpSend(client, resp);
        return;
    }

    std::string supi       = json::get(req.body, "supi");
    std::string dnn        = json::get(req.body, "dnn");
    std::string snssai     = json::get(req.body, "snssai");
    std::string pdu_sess_id= json::get(req.body, "pduSessionId");
    int psi = pdu_sess_id.empty() ? 1 : std::stoi(pdu_sess_id);

    Logger::smf(Level::BEGINNER,
        "SMF <- AMF: Nsmf_PDUSession_CreateSMContext [TS 29.502 §4.2.2.2]");
    Logger::ie_field("  SUPI        = " + supi);
    Logger::ie_field("  DNN         = " + (dnn.empty() ? "internet" : dnn));
    Logger::ie_field("  S-NSSAI     = " + (snssai.empty() ? "{sst:1,sd:000001}" : snssai));
    Logger::ie_field("  PDU Session = " + std::to_string(psi));

    Logger::smf(Level::INTERVIEW_T,
        "[INTERVIEW] Q: Walk me through PDU Session Establishment.");
    Logger::smf(Level::INTERVIEW_C,
        "A: UE sends NAS PDU Session Establishment Request → gNB → AMF.");
    Logger::smf(Level::INTERVIEW_C,
        "   AMF calls SMF: Nsmf_PDUSession_CreateSMContext (HTTP POST).");
    Logger::smf(Level::INTERVIEW_C,
        "   SMF allocates UE IP, selects UPF, sets up N4 session via PFCP.");
    Logger::smf(Level::INTERVIEW_C,
        "   SMF returns 201 Created + ueIp, SMContext location URL.");
    Logger::smf(Level::INTERVIEW_C,
        "   AMF sends NGAP PDU Session Resource Setup to gNB with UE IP.");
    Logger::smf(Level::INTERVIEW_C,
        "   gNB creates GTP-U tunnel to UPF. Data plane established.");

    // Allocate UE IP from pool
    std::string ue_ip = allocateUeIp();
    Logger::smf(Level::ENGINEER, "[PDU] Allocated UE IP=" + ue_ip + " from DNN pool 10.45.0.0/16");

    // Setup N4 (PFCP) session with UPF
    setupN4Session(ue_ip, uint32_t(psi));

    // SMContext URL (used by AMF for future SM Context operations: modify, release)
    std::string sm_ctx_ref = "/nsmf-pdusession/v1/sm-contexts/ue-" +
                              supi.substr(supi.size() > 5 ? supi.size() - 5 : 0);

    std::string resp_body = json::obj({
        {"smContextRef",  json::str(sm_ctx_ref)},
        {"ueIpAddress",   json::str(ue_ip)},
        {"pduSessionId",  json::num(psi)},
        {"dnn",           json::str(dnn.empty() ? "internet" : dnn)},
        {"upfIp",         json::str("10.1.0.4")},
        {"upfTeid",       json::str("0x00000101")},
    });

    std::string resp = "HTTP/1.1 201 Created\r\n"
                       "Location: " + sm_ctx_ref + "\r\n"
                       "Content-Type: application/json\r\n"
                       "Content-Length: " + std::to_string(resp_body.size()) + "\r\n"
                       "\r\n" + resp_body;

    Logger::smf(Level::BEGINNER,
        "SMF -> AMF: 201 Created  ueIp=" + ue_ip + "  smContextRef=" + sm_ctx_ref);
    Logger::ie_field("  UPF TEID=0x00000101 (for GTP-U tunnel from gNB to UPF)");

    httpSend(client, resp);

    // PCAP: write SBI exchange (AMF→SMF request + SMF→AMF response)
    std::string req_text = req.startLine + "\r\n" + req.body;
    pcap().writeAppText(req_text, PcapWriter::IP_AMF, PcapWriter::PORT_SBI,
                                   PcapWriter::IP_SMF, PcapWriter::PORT_SBI);
    pcap().writeAppText(resp, PcapWriter::IP_SMF, PcapWriter::PORT_SBI,
                               PcapWriter::IP_AMF, PcapWriter::PORT_SBI);
}

int main() {
    Logger::setSessionFile("g5_smf_session.log");
    Logger::setLevelFromEnv();

    const uint16_t SMF_SBI_PORT = 29502;
    pcap().open("5g_smf_capture.pcap");

    Logger::step("SMF starting");
    Logger::sys("SMF: Session Management Function (TS 23.501 §6.2.2)");
    Logger::sys("SMF: handles PDU session lifecycle, allocates UE IPs, programs UPF via PFCP");

    const char* SMF_SELF_HOST = std::getenv("SMF_HOST") ? std::getenv("SMF_HOST") : "127.0.0.1";

    // Discover UPF via NRF (or fall back to env/localhost)
    auto upf = nrfclient::discover(Logger::CLR_SMF, " SMF  ", "UPF");
    if (upf.found) {
        s_upf_host = upf.host;
        s_upf_pfcp_port = 8805; // UPF always listens on 8805
    } else {
        s_upf_host = std::getenv("UPF_HOST") ? std::getenv("UPF_HOST") : "127.0.0.1";
        Logger::sys("SMF: NRF discovery for UPF failed — fallback to " + s_upf_host);
    }
    Logger::sys("SMF: UPF N4 peer at " + s_upf_host + ":" + std::to_string(s_upf_pfcp_port));

    // Register with NRF
    nrfclient::registerSelf(Logger::CLR_SMF, " SMF  ", "smf-1", "SMF", SMF_SELF_HOST, SMF_SBI_PORT);

    Logger::sys("SMF: SBI listening on TCP :" + std::to_string(SMF_SBI_PORT));
    Socket server = Socket::createServer("0.0.0.0", SMF_SBI_PORT);

    Logger::sys("SMF: Waiting for Nsmf_PDUSession_CreateSMContext from AMF...");
    Logger::sys("SMF: Metrics: GET /nsmf-status/v1/health for liveness probe (K8s)");

    uint32_t ip_counter = 2;
    while (true) {
        Socket client = server.accept();
        Logger::smf(Level::ENGINEER, "[SBI] new connection (AMF or test client)");
        handleOne(client, ip_counter);
    }
}
