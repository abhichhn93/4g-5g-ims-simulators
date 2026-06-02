#include "ims/ims_hss.h"
#include "common/logger.h"
#include <stdexcept>

static constexpr const char* HSS_CX_IP   = "127.0.0.1";
static constexpr uint16_t    HSS_CX_PORT  = 3870;

ImsHssNode::ImsHssNode(std::atomic<bool>& stop, std::atomic<bool>& ims_hss_ready)
    : stop_(stop), ims_hss_ready_(ims_hss_ready)
{}

void ImsHssNode::run() {
    Logger::hss("IMS-HSS: thread started — Cx interface on port " + std::to_string(HSS_CX_PORT));
    Logger::hss("IMS-HSS: REAL: same physical HSS as EPS HSS (S6a for 4G attach, Cx for IMS)");
    Logger::hss("IMS-HSS: Stores: IMPU↔IMPI mapping, S-CSCF assignment, IMS subscription profile");
    try {
        setupServer();
        if (!stop_.load()) receiveLoop();
    } catch (const std::exception& e) {
        Logger::warn("IMS-HSS", e.what());
    }
    Logger::hss("IMS-HSS: thread exiting");
}

void ImsHssNode::setupServer() {
    server_socket_ = Socket::createServer(HSS_CX_IP, HSS_CX_PORT);
    ims_hss_ready_.store(true);
    Logger::hss("IMS-HSS: ready — waiting for S-CSCF");
    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            scscf_conn_ = server_socket_.accept();
            Logger::hss("IMS-HSS: S-CSCF connected (Cx link UP) ✓");
            return;
        }
    }
}

void ImsHssNode::receiveLoop() {
    while (!stop_.load()) {
        if (!scscf_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!scscf_conn_.recvFrame(payload)) break;
        MessageReader r(payload);
        auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));
        if (type == SipMsgType::DIA_CX_SAR) handleSAR(payload);
    }
}

void ImsHssNode::handleSAR(const std::vector<uint8_t>& payload) {
    std::string impu, impi;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::CX_IMPU)))
            impu = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::CX_IMPI)))
            impi = r.readStr();
        else r.skip();
    }

    Logger::hss("IMS-HSS: ← Diameter Cx SAR from S-CSCF [TS 29.229 §6.1.3]");
    Logger::ie_field("  IMPU: " + impu);
    Logger::ie_field("  IMPI: " + impi);
    Logger::hss("IMS-HSS: Storing: IMPU → S-CSCF assignment");
    Logger::hss("IMS-HSS: Building subscriber profile (service profile + iFC)");
    Logger::ie_field("  iFC (Initial Filter Criteria): defines when to invoke MTAS");
    Logger::ie_field("  iFC example: trigger MTAS on REGISTER (Third Party Reg)");
    Logger::ie_field("  iFC example: trigger MTAS on INVITE (call service logic)");
    Logger::ie_field("  MSISDN: +919...(phone number associated with IMPU)");
    Logger::hss("IMS-HSS: → Diameter Cx SAA to S-CSCF");

    // Build SAA response
    MessageWriter w(static_cast<MessageType>(static_cast<uint16_t>(SipMsgType::DIA_CX_SAA)),
                    next_seq_++);
    std::string profile = "MMTEL-VoLTE; iFC=[REGISTER->MTAS,INVITE->MTAS]; MSISDN=+919000000001";
    w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::CX_SCSCF_NAME)), profile);
    scscf_conn_.sendFrame(w.frame());
    Logger::hss("IMS-HSS: SAA sent — subscriber profile delivered to S-CSCF ✓");
}
