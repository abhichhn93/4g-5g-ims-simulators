#include "ims/pcscf_node.h"
#include "common/logger.h"
#include <stdexcept>
#include <chrono>
#include <thread>

static constexpr const char* PCSCF_IP   = "127.0.0.1";
static constexpr uint16_t    PCSCF_PORT  = 5060;
static constexpr const char* SCSCF_IP   = "127.0.0.1";
static constexpr uint16_t    SCSCF_PORT  = 5070;

PcscfNode::PcscfNode(std::atomic<bool>& stop, std::atomic<bool>& pcscf_ready)
    : stop_(stop), pcscf_ready_(pcscf_ready)
{}

void PcscfNode::run() {
    Logger::pcscf("P-CSCF: thread started");
    Logger::pcscf("P-CSCF: REAL: First IMS contact point. UE discovers P-CSCF via");
    Logger::pcscf("P-CSCF:   PCO (Protocol Config Options) in 4G PDN Connectivity Response.");
    Logger::pcscf("P-CSCF:   In Ericsson deployment: P-CSCF is often SBC (Session Border Controller).");
    try {
        setupServer();
        if (!stop_.load()) receiveLoop();
    } catch (const std::exception& e) {
        Logger::warn("P-CSCF", e.what());
    }
    Logger::pcscf("P-CSCF: thread exiting");
}

void PcscfNode::setupServer() {
    Logger::pcscf("P-CSCF: binding TCP on port " + std::to_string(PCSCF_PORT) + " (SIP standard port)");
    Logger::pcscf("P-CSCF: REAL: uses UDP or TCP 5060, TLS 5061");
    server_socket_ = Socket::createServer(PCSCF_IP, PCSCF_PORT);
    pcscf_ready_.store(true);

    Logger::pcscf("P-CSCF: connecting to S-CSCF on port " + std::to_string(SCSCF_PORT));
    // Wait for S-CSCF to be ready
    for (int i = 0; i < 50 && !stop_.load(); ++i) {
        try { scscf_conn_ = Socket::connectTo(SCSCF_IP, SCSCF_PORT); break; }
        catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    Logger::pcscf("P-CSCF: S-CSCF connected ✓");

    Logger::pcscf("P-CSCF: waiting for UE to connect...");
    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            ue_conn_ = server_socket_.accept();
            Logger::pcscf("P-CSCF: UE connected ✓ — ready for SIP");
            return;
        }
    }
}

void PcscfNode::receiveLoop() {
    Logger::pcscf("P-CSCF: receive loop started");
    while (!stop_.load()) {
        // Check from UE
        if (ue_conn_.valid() && ue_conn_.hasData(50)) {
            std::vector<uint8_t> payload;
            if (!ue_conn_.recvFrame(payload)) break;
            MessageReader r(payload);
            auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));
            switch (type) {
                case SipMsgType::SIP_REGISTER: handleRegister(payload); break;
                case SipMsgType::SIP_INVITE:   handleInvite(payload);   break;
                case SipMsgType::SIP_ACK:
                case SipMsgType::SIP_BYE:      handleBye(payload);      break;
                default: break;
            }
        }
        // Check from S-CSCF (responses going back to UE)
        if (scscf_conn_.valid() && scscf_conn_.hasData(50)) {
            std::vector<uint8_t> payload;
            if (!scscf_conn_.recvFrame(payload)) break;
            MessageReader r(payload);
            auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));
            if (type == SipMsgType::SIP_200_OK)      handle200Ok(payload);
            else if (type == SipMsgType::SIP_180_RINGING) handle180Ringing(payload);
            else forwardToUe(payload);
        }
    }
}

void PcscfNode::handleRegister(const std::vector<uint8_t>& payload) {
    MessageReader r(payload);
    std::string from, impu, impi, contact;
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_FROM)))
            from = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_IMPU)))
            impu = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_IMPI)))
            impi = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CONTACT)))
            contact = r.readStr();
        else r.skip();
    }

    Logger::pcscf("P-CSCF: ← SIP REGISTER from UE");
    Logger::ie_field("  From: " + from);
    Logger::ie_field("  IMPU: " + impu + "  (IMS Public Identity — like a phone number URI)");
    Logger::ie_field("  IMPI: " + impi + "  (IMS Private Identity — like username for auth)");
    Logger::ie_field("  Contact: " + contact + "  (UE's IP:port for media)");
    Logger::pcscf("P-CSCF: REAL: adds Via header, checks auth (IMS-AKA), adds Route header");
    Logger::pcscf("P-CSCF: REAL: discovers I-CSCF via DNS NAPTR query on home domain");
    Logger::pcscf("P-CSCF: → forwarding REGISTER to S-CSCF (via I-CSCF in real network)");

    forwardToSCscf(payload);
}

void PcscfNode::handleInvite(const std::vector<uint8_t>& payload) {
    MessageReader r(payload);
    std::string from, to, call_id, sdp;
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_FROM)))
            from = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_TO)))
            to = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CALL_ID)))
            call_id = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_SDP)))
            sdp = r.readStr();
        else r.skip();
    }

    Logger::pcscf("P-CSCF: ← SIP INVITE from UE (VoLTE call initiation)");
    Logger::ie_field("  From: " + from);
    Logger::ie_field("  To:   " + to);
    Logger::ie_field("  Call-ID: " + call_id);
    Logger::ie_field("  SDP: " + sdp);
    Logger::pcscf("P-CSCF: REAL: validates From URI matches registered contact");
    Logger::pcscf("P-CSCF: REAL: applies preconditions check (RFC 3312) for QoS signalling");
    Logger::pcscf("P-CSCF: → forwarding INVITE to S-CSCF");

    forwardToSCscf(payload);
}

void PcscfNode::handle200Ok(const std::vector<uint8_t>& payload) {
    MessageReader r(payload);
    std::string reason, call_id;
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_REASON)))
            reason = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CALL_ID)))
            call_id = r.readStr();
        else r.skip();
    }
    Logger::pcscf("P-CSCF: ← SIP 200 OK from S-CSCF (" + reason + ")");
    if (reason == "INVITE") {
        Logger::pcscf("P-CSCF: REAL: This is the key moment — media is now negotiated!");
        Logger::pcscf("P-CSCF: → Diameter Rx AAR to PCRF — requesting QCI=1 dedicated bearer");
        Logger::pcscf("P-CSCF: REAL: Rx carries media info (codec, bandwidth, IP:port from SDP)");
        Logger::pcscf("P-CSCF: PCRF → P-GW: Gx RAR → creates QCI=1 bearer for voice");
        Logger::pcscf("P-CSCF: QCI=1 = Conversational Voice (lowest latency, highest priority)");
        sendRxAAR("registered-user", call_id);
    }
    forwardToUe(payload);
}

void PcscfNode::handle180Ringing(const std::vector<uint8_t>& payload) {
    Logger::pcscf("P-CSCF: ← SIP 180 Ringing — forwarding to UE");
    Logger::pcscf("P-CSCF: REAL: UE plays ringback tone. PRACK may be sent for reliability.");
    forwardToUe(payload);
}

void PcscfNode::handleBye(const std::vector<uint8_t>& payload) {
    Logger::pcscf("P-CSCF: ← SIP BYE — forwarding to S-CSCF");
    Logger::pcscf("P-CSCF: After BYE completes: Diameter Rx STR to PCRF → release QCI=1 bearer");
    forwardToSCscf(payload);
}

void PcscfNode::sendRxAAR(const std::string& impu, const std::string& call_id) {
    Logger::pcscf("P-CSCF: → Diameter Rx AAR to PCRF [TS 29.214]");
    Logger::ie_field("  User: " + impu + "  Call-ID: " + call_id);
    Logger::ie_field("  Media: audio, codec=AMR-WB, bandwidth=12.65kbps, direction=sendrecv");
    Logger::ie_field("  Requesting: QCI=1 (Voice), ARP=2 (preemption capability)");
    // In Phase 5: actually send to PCRF via TCP
    Logger::pcscf("P-CSCF: REAL: PCRF matches this to existing Gx session, installs QCI=1 rule");
    Logger::pcscf("P-CSCF: REAL: P-GW sends Create Bearer Request → S-GW → MME → eNB");
    Logger::pcscf("P-CSCF: REAL: eNB sets up DRB (Data Radio Bearer) with QCI=1 priority");
}

void PcscfNode::forwardToSCscf(const std::vector<uint8_t>& frame) {
    if (scscf_conn_.valid()) scscf_conn_.sendFrame(frame);
}

void PcscfNode::forwardToUe(const std::vector<uint8_t>& frame) {
    if (ue_conn_.valid()) ue_conn_.sendFrame(frame);
}
