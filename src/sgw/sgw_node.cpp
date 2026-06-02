#include "sgw/sgw_node.h"
#include "common/logger.h"
#include "common/pcap_writer.h"
#include <stdexcept>
#include <cstdio>

static constexpr const char* SGW_IP  = "127.0.0.1";
static constexpr uint16_t    SGW_PORT = 2123;  // GTP-C S11 port (real standard)
static constexpr const char* PGW_IP  = "127.0.0.1";
static constexpr uint16_t    PGW_PORT = 2124;

SgwNode::SgwNode(std::atomic<bool>& stop, std::atomic<bool>& sgw_ready)
    : stop_(stop), sgw_ready_(sgw_ready)
{}

void SgwNode::run() {
    Logger::sys("SGW: thread started");
    try {
        setupSockets();
        if (!stop_.load()) receiveLoop();
    } catch (const std::exception& e) {
        Logger::warn("SGW", e.what());
    }
    Logger::sys("SGW: thread exiting");
}

void SgwNode::setupSockets() {
    Logger::sys("SGW: binding UDP S11 socket on " + std::string(SGW_IP) + ":" + std::to_string(SGW_PORT));
    Logger::sys("SGW: REAL: S11 uses GTP-C (TS 29.274) over UDP. Real S-GWs may use SCTP for control plane.");
    Logger::sys("SGW: S5 socket is unbound — we use sendto/recvfrom to talk to P-GW on port " + std::to_string(PGW_PORT));

    s11_socket_ = UdpSocket::bind(SGW_IP, SGW_PORT);
    s5_socket_  = UdpSocket::bind(SGW_IP, 0);  // OS assigns ephemeral port for S5

    sgw_ready_.store(true);
    Logger::sys("SGW: ready ✓ — waiting for GTP-C from MME");
}

void SgwNode::receiveLoop() {
    Logger::sys("SGW: entering receive loop on UDP port " + std::to_string(SGW_PORT));
    while (!stop_.load()) {
        if (!s11_socket_.hasData(100)) continue;

        std::vector<uint8_t> payload;
        sockaddr_in mme_addr{};
        if (!s11_socket_.recvFrom(payload, mme_addr)) continue;

        if (payload.size() < 8) continue;
        MessageReader r(payload);

        switch (r.msgType()) {
            case MessageType::GTP_CREATE_SESSION_REQ:
                handleCreateSessionReq(payload, mme_addr);
                break;
            case MessageType::GTP_MODIFY_BEARER_REQ:
                handleModifyBearerReq(payload, mme_addr);
                break;
            default:
                Logger::warn("SGW", "unexpected: " + std::string(msg_type_str(r.msgType())));
        }
    }
}

void SgwNode::handleCreateSessionReq(const std::vector<uint8_t>& payload, const sockaddr_in& mme_addr) {
    // Parse MME's Create Session Request (S11)
    uint64_t imsi = 0;
    std::string apn;
    uint8_t qci = 9, ebi = 5;
    uint32_t ambr_ul = 50000000, ambr_dl = 100000000;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::GTP_IMSI:    imsi     = r.readU64(); break;
            case Tag::GTP_APN:     apn      = r.readStr(); break;
            case Tag::GTP_EBI:     ebi      = r.readU8();  break;
            case Tag::GTP_QCI:     qci      = r.readU8();  break;
            case Tag::GTP_AMBR_UL: ambr_ul  = r.readU32(); break;
            case Tag::GTP_AMBR_DL: ambr_dl  = r.readU32(); break;
            default:               r.skip(); break;
        }
    }

    Logger::sys("SGW: ← RECV GTP-C CreateSessionReq [TS 29.274 §7.2.1] from MME");
    Logger::ie_field("  IMSI=" + std::to_string(imsi) + "  APN=" + apn);
    Logger::ie_field("  EBI=" + std::to_string(ebi) + "  QCI=" + std::to_string(qci) +
                     "  AMBR: UL=" + std::to_string(ambr_ul/1000000) + "Mbps DL=" + std::to_string(ambr_dl/1000000) + "Mbps");

    // Allocate S-GW TEIDs
    uint32_t sgw_s11_teid = next_teid_.fetch_add(1);  // atomic: no lock needed
    uint32_t sgw_s1u_teid = next_teid_.fetch_add(1);  // data plane TEID for eNB tunnel
    Logger::sys("SGW: allocated TEIDs — S11-ctrl=" + std::to_string(sgw_s11_teid) +
                " S1U-data=" + std::to_string(sgw_s1u_teid));
    Logger::sys("SGW: → FORWARD CreateSessionReq to P-GW via S5 interface (UDP port " + std::to_string(PGW_PORT) + ")");
    PcapWriter::instance().writeGTPv2(GtpMsgType::CREATE_SESSION_REQ, 0,
        PcapWriter::IP_SGW, PcapWriter::PORT_SGW, PcapWriter::IP_PGW, PcapWriter::PORT_PGW);

    // Forward Create Session Request to P-GW (S5 interface)
    MessageWriter fwd(MessageType::GTP_CREATE_SESSION_REQ, next_seq_++);
    fwd.writeU64(Tag::GTP_IMSI,        imsi);
    fwd.writeStr(Tag::GTP_APN,         apn);
    fwd.writeU8 (Tag::GTP_EBI,         ebi);
    fwd.writeU8 (Tag::GTP_QCI,         qci);
    fwd.writeU32(Tag::GTP_SENDER_TEID, sgw_s11_teid); // S-GW's S5 control TEID
    fwd.writeU32(Tag::GTP_AMBR_UL,     ambr_ul);
    fwd.writeU32(Tag::GTP_AMBR_DL,     ambr_dl);
    s5_socket_.sendTo(fwd.udpPayload(), PGW_IP, PGW_PORT);

    // Wait for P-GW's Create Session Response (synchronous — Phase 3 single UE)
    Logger::sys("SGW: waiting for P-GW CreateSessionRsp...");
    std::vector<uint8_t> pgw_resp;
    sockaddr_in pgw_addr{};
    if (!s5_socket_.recvWithTimeout(pgw_resp, pgw_addr, 5000)) {
        Logger::warn("SGW", "P-GW CreateSessionRsp timeout!");
        return;
    }

    // Parse P-GW's response
    uint8_t  cause = 0;
    uint32_t pgw_s5_teid = 0; (void)pgw_s5_teid;
    std::vector<uint8_t> ue_ip_bytes;

    MessageReader pr(pgw_resp);
    while (pr.hasMore()) {
        Tag tag; uint16_t len;
        if (!pr.peek(tag, len)) break;
        switch (tag) {
            case Tag::GTP_CAUSE:       cause       = pr.readU8();    break;
            case Tag::GTP_SENDER_TEID: pgw_s5_teid = pr.readU32();  break;
            case Tag::GTP_UE_IP:       ue_ip_bytes = pr.readBytes(); break;
            default:                   pr.skip();                    break;
        }
    }

    if (cause != 16 || ue_ip_bytes.size() < 4) {
        Logger::warn("SGW", "P-GW CreateSessionRsp error cause=" + std::to_string(cause));
        return;
    }

    char ue_ip_str[32];
    std::snprintf(ue_ip_str, sizeof(ue_ip_str), "%d.%d.%d.%d",
                  ue_ip_bytes[0], ue_ip_bytes[1], ue_ip_bytes[2], ue_ip_bytes[3]);

    Logger::sys("SGW: ← RECV GTP-C CreateSessionRsp from P-GW — UE IP=" + std::string(ue_ip_str));
    PcapWriter::instance().writeGTPv2(GtpMsgType::CREATE_SESSION_RSP, pgw_s5_teid,
        PcapWriter::IP_PGW, PcapWriter::PORT_PGW, PcapWriter::IP_SGW, PcapWriter::PORT_SGW);
    Logger::sys("SGW: → SEND GTP-C CreateSessionRsp to MME (with S-GW's TEIDs + UE IP)");
    PcapWriter::instance().writeGTPv2(GtpMsgType::CREATE_SESSION_RSP, sgw_s11_teid,
        PcapWriter::IP_SGW, PcapWriter::PORT_SGW, PcapWriter::IP_MME, PcapWriter::PORT_SGW);

    // Send Create Session Response to MME (with S-GW's TEIDs + UE IP from P-GW)
    MessageWriter rsp(MessageType::GTP_CREATE_SESSION_RSP, next_seq_++);
    rsp.writeU8 (Tag::GTP_CAUSE,       16);             // 16 = Request Accepted
    rsp.writeU32(Tag::GTP_SENDER_TEID, sgw_s11_teid);   // S-GW's S11 control TEID
    rsp.writeU32(Tag::GTP_BEARER_TEID, sgw_s1u_teid);   // S-GW's S1-U data TEID
    rsp.writeU64(Tag::GTP_IMSI,        imsi);
    rsp.writeBytes(Tag::GTP_UE_IP, ue_ip_bytes.data(), 4); // UE's IP from P-GW
    s11_socket_.sendToAddr(rsp.udpPayload(), mme_addr);

    Logger::ie_field("  S-GW S11-ctrl TEID=" + std::to_string(sgw_s11_teid) +
                     "  S-GW S1U-data TEID=" + std::to_string(sgw_s1u_teid) +
                     "  UE IP=" + std::string(ue_ip_str));
    Logger::sys("SGW: CreateSession flow complete ✓");
}

void SgwNode::handleModifyBearerReq(const std::vector<uint8_t>& payload, const sockaddr_in& mme_addr) {
    uint32_t enb_teid = 0;
    uint64_t imsi = 0;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::GTP_ENB_TEID: enb_teid = r.readU32(); break;
            case Tag::GTP_IMSI:     imsi      = r.readU64(); break;
            default:                r.skip();                break;
        }
    }

    Logger::sys("SGW: ← RECV GTP-C ModifyBearerReq [TS 29.274 §7.2.7]");
    Logger::ie_field("  eNB S1-U TEID=" + std::to_string(enb_teid) + "  IMSI=" + std::to_string(imsi));
    Logger::sys("SGW: REAL: S-GW updates its downlink routing table:");
    Logger::sys("SGW:   When P-GW sends downlink packet → S-GW looks up eNB TEID → encapsulates in GTP-U → sends to eNB");
    Logger::sys("SGW: → SEND GTP-C ModifyBearerRsp to MME");
    PcapWriter::instance().writeGTPv2(GtpMsgType::MODIFY_BEARER_REQ, 0,
        PcapWriter::IP_MME, PcapWriter::PORT_SGW, PcapWriter::IP_SGW, PcapWriter::PORT_SGW);
    PcapWriter::instance().writeGTPv2(GtpMsgType::MODIFY_BEARER_RSP, 0,
        PcapWriter::IP_SGW, PcapWriter::PORT_SGW, PcapWriter::IP_MME, PcapWriter::PORT_SGW);

    MessageWriter rsp(MessageType::GTP_MODIFY_BEARER_RSP, next_seq_++);
    rsp.writeU8(Tag::GTP_CAUSE, 16);  // success
    s11_socket_.sendToAddr(rsp.udpPayload(), mme_addr);
}
