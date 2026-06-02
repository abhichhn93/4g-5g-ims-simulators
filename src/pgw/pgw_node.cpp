#include "pgw/pgw_node.h"
#include "common/pcap_writer.h"
#include "common/logger.h"
#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <thread>

static constexpr const char* PGW_IP    = "127.0.0.1";
static constexpr uint16_t    PGW_PORT   = 2124;
static constexpr const char* PCRF_IP   = "127.0.0.1";
static constexpr uint16_t    PCRF_PORT  = 3869;

PgwNode::PgwNode(std::atomic<bool>& stop, std::atomic<bool>& pgw_ready,
                 std::atomic<bool>& pcrf_ready)
    : stop_(stop), pgw_ready_(pgw_ready), pcrf_ready_(pcrf_ready)
{}

void PgwNode::run() {
    Logger::sys("PGW: thread started");
    try {
        setupSocket();
        if (!stop_.load()) receiveLoop();
    } catch (const std::exception& e) {
        Logger::warn("PGW", e.what());
    }
    Logger::sys("PGW: thread exiting");
}

void PgwNode::setupSocket() {
    Logger::sys("PGW: binding UDP S5 socket on " + std::string(PGW_IP) + ":" + std::to_string(PGW_PORT));
    Logger::sys("PGW: IP pool: 10.0.0.1 – 10.0.254.254");
    s5_socket_ = UdpSocket::bind(PGW_IP, PGW_PORT);
    pgw_ready_.store(true);

    // Wait for PCRF to be ready then connect (Gx interface)
    Logger::sys("PGW: waiting for PCRF to be ready...");
    while (!pcrf_ready_.load() && !stop_.load())
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop_.load()) return;

    Logger::sys("PGW: connecting to PCRF on TCP " + std::string(PCRF_IP) + ":" + std::to_string(PCRF_PORT));
    Logger::sys("PGW: REAL: Diameter Gx over SCTP/TCP to PCRF, port 3868 (we use 3869)");
    pcrf_conn_ = Socket::connectTo(PCRF_IP, PCRF_PORT);
    pcrf_connected_ = true;
    Logger::sys("PGW: Gx link to PCRF UP ✓");
    Logger::sys("PGW: ready — UDP S5 on 2124, Gx TCP to PCRF");
}

bool PgwNode::sendCCRandWaitCCA(uint64_t imsi, const std::string& apn, uint8_t qci) {
    // ── Send Diameter Gx CCR-I to PCRF ───────────────────────
    Logger::sys("PGW: → Diameter Gx CCR-I to PCRF [TS 29.212 §4.5.1]");
    Logger::ie_field("  IMSI=" + std::to_string(imsi) + "  APN=" + apn + "  QCI=" + std::to_string(qci));
    Logger::sys("PGW: REAL: CCR-I also includes: Framed-IP-Address, Event-Trigger=SESSION_START");
    Logger::sys("PGW: REAL: PCRF matches IMSI to subscriber policy (rate limits, allowed services)");

    MessageWriter ccr(MessageType::DIA_GX_CCR_I, next_seq_++);
    ccr.writeU64(Tag::GX_IMSI, imsi);
    ccr.writeStr(Tag::GX_APN,  apn);
    ccr.writeU8 (Tag::GX_QCI,  qci);
    pcrf_conn_.sendFrame(ccr.frame());

    // ── Wait for Diameter Gx CCA-I from PCRF ─────────────────
    std::vector<uint8_t> payload;
    if (!pcrf_conn_.hasData(3000)) {
        Logger::warn("PGW", "Gx CCA-I timeout from PCRF");
        return false;
    }
    if (!pcrf_conn_.recvFrame(payload)) {
        Logger::warn("PGW", "Gx CCA-I receive failed");
        return false;
    }
    MessageReader r(payload);
    if (r.msgType() != MessageType::DIA_GX_CCA_I) {
        Logger::warn("PGW", "expected CCA-I, got " + std::string(msg_type_str(r.msgType())));
        return false;
    }

    std::string rule_name; uint8_t result = 0;
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::GX_RESULT_CODE: result    = r.readU8();  break;
            case Tag::GX_RULE_NAME:   rule_name = r.readStr(); break;
            default:                  r.skip();                break;
        }
    }

    Logger::sys("PGW: ← Diameter Gx CCA-I from PCRF");
    Logger::ie_field("  Result=OK  Charging-Rule='" + rule_name + "'");
    Logger::sys("PGW: REAL: P-GW installs the charging rule in its PCEF (Policy and Charging");
    Logger::sys("PGW:   Enforcement Function). All UE packets now matched against this rule.");
    return (result == 1);
}

void PgwNode::handleCreateSessionReq(const std::vector<uint8_t>& payload, const sockaddr_in& sgw_addr) {
    uint64_t    imsi = 0;
    std::string apn;
    uint8_t     qci = 9, ebi = 5;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::GTP_IMSI: imsi = r.readU64(); break;
            case Tag::GTP_APN:  apn  = r.readStr(); break;
            case Tag::GTP_EBI:  ebi  = r.readU8();  break;
            case Tag::GTP_QCI:  qci  = r.readU8();  break;
            default:            r.skip();            break;
        }
    }

    Logger::sys("PGW: ← GTP-C CreateSessionReq from S-GW  IMSI=" + std::to_string(imsi) + " APN=" + apn);
    PcapWriter::instance().writeGTPv2(GtpMsgType::CREATE_SESSION_REQ, 0,
        PcapWriter::IP_SGW, PcapWriter::PORT_PGW, PcapWriter::IP_PGW, PcapWriter::PORT_PGW);

    // ── Gx: Get policy from PCRF before creating session ─────
    // DESIGN: P-GW can't create a bearer without PCRF approval.
    // If PCRF denies (e.g., subscriber blacklisted, balance zero), P-GW
    // sends Create Session Response with cause=REQUEST_DENIED.
    if (pcrf_connected_) {
        if (!sendCCRandWaitCCA(imsi, apn.empty() ? "internet" : apn, qci)) {
            Logger::warn("PGW", "PCRF denied — sending reject to S-GW");
            MessageWriter rsp(MessageType::GTP_CREATE_SESSION_RSP, next_seq_++);
            rsp.writeU8(Tag::GTP_CAUSE, 64);  // 64 = Request Not Accepted
            s5_socket_.sendToAddr(rsp.udpPayload(), sgw_addr);
            return;
        }
    }

    // ── Allocate UE IP and P-GW TEID ──────────────────────────
    std::string ue_ip   = allocateIP();
    uint32_t pgw_teid   = next_teid_.fetch_add(1, std::memory_order_relaxed);

    Logger::sys("PGW: ✓ session created — UE IP=" + ue_ip + "  P-GW TEID=" + std::to_string(pgw_teid));
    Logger::ie_field("  UE connected to internet via P-GW (SGi interface: P-GW ↔ Internet)");
    Logger::ie_field("  REAL: P-GW also programs the GTP-U forwarding table for user data packets");

    unsigned a,b,c,d;
    uint8_t ip_b[4] = {0,0,0,0};
    if (std::sscanf(ue_ip.c_str(), "%u.%u.%u.%u", &a,&b,&c,&d) == 4) {
        ip_b[0]=uint8_t(a); ip_b[1]=uint8_t(b); ip_b[2]=uint8_t(c); ip_b[3]=uint8_t(d);
    }

    MessageWriter rsp(MessageType::GTP_CREATE_SESSION_RSP, next_seq_++);
    rsp.writeU8   (Tag::GTP_CAUSE,       16);
    rsp.writeU32  (Tag::GTP_SENDER_TEID, pgw_teid);
    rsp.writeBytes(Tag::GTP_UE_IP,       ip_b, 4);
    rsp.writeU8   (Tag::GTP_EBI,         ebi);
    s5_socket_.sendToAddr(rsp.udpPayload(), sgw_addr);
    PcapWriter::instance().writeGTPv2(GtpMsgType::CREATE_SESSION_RSP, pgw_teid,
        PcapWriter::IP_PGW, PcapWriter::PORT_PGW, PcapWriter::IP_SGW, PcapWriter::PORT_SGW);
}

void PgwNode::receiveLoop() {
    Logger::sys("PGW: receive loop started on UDP port " + std::to_string(PGW_PORT));
    while (!stop_.load()) {
        if (!s5_socket_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        sockaddr_in sgw_addr{};
        if (!s5_socket_.recvFrom(payload, sgw_addr)) continue;
        if (payload.size() < 8) continue;
        MessageReader r(payload);
        if (r.msgType() == MessageType::GTP_CREATE_SESSION_REQ)
            handleCreateSessionReq(payload, sgw_addr);
        else
            Logger::warn("PGW", "unexpected: " + std::string(msg_type_str(r.msgType())));
    }
}

std::string PgwNode::allocateIP() {
    // Atomic: no mutex needed — fetch_add is lock-free
    uint32_t raw = next_ip_.fetch_add(1, std::memory_order_relaxed);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
                  (raw>>24)&0xFF,(raw>>16)&0xFF,(raw>>8)&0xFF,raw&0xFF);
    return buf;
}
