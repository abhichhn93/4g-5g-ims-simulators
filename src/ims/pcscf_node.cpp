#include "ims/pcscf_node.h"
#include "common/logger.h"
#include "common/pcap_writer.h"
#include "common/visual_logger.h"
#include <chrono>
#include <thread>
#include <stdexcept>
#include <sstream>

static constexpr const char* PCSCF_IP  = "127.0.0.1";
static constexpr uint16_t    PCSCF_PORT = 5060;
static constexpr const char* SCSCF_IP  = "127.0.0.1";
static constexpr uint16_t    SCSCF_PORT = 5070;

PcscfNode::PcscfNode(std::atomic<bool>& stop, std::atomic<bool>& pcscf_ready)
    : stop_(stop), pcscf_ready_(pcscf_ready)
{}

void PcscfNode::run() {
    Logger::pcscf("P-CSCF: starting — multi-UE TCP server on port " + std::to_string(PCSCF_PORT));
    Logger::pcscf("P-CSCF: REAL: First SIP contact. UE-A, UE-B, UE-C each connect here.");
    try {
        connectToSCscf();
        server_socket_ = Socket::createServer(PCSCF_IP, PCSCF_PORT);
        pcscf_ready_.store(true);
        Logger::pcscf("P-CSCF: ready — waiting for UE-A, UE-B, UE-C to connect...");

        // S-CSCF receive thread
        std::thread scscf_th([this]{ scscfReceiveLoop(); });

        // Accept UE connections loop (runs on this thread)
        acceptLoop();

        scscf_th.join();
        for (auto& t : ue_threads_) if (t.joinable()) t.join();

    } catch (const std::exception& e) {
        Logger::warn("P-CSCF", e.what());
    }
    Logger::pcscf("P-CSCF: thread exiting");
}

void PcscfNode::connectToSCscf() {
    Logger::pcscf("P-CSCF: connecting to S-CSCF on port " + std::to_string(SCSCF_PORT));
    for (int i = 0; i < 50 && !stop_.load(); ++i) {
        try { scscf_conn_ = Socket::connectTo(SCSCF_IP, SCSCF_PORT); break; }
        catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    Logger::pcscf("P-CSCF: S-CSCF connected ✓");
}

void PcscfNode::acceptLoop() {
    while (!stop_.load()) {
        if (!server_socket_.hasConnection(200)) continue;
        Socket ue_sock = server_socket_.accept();
        Logger::pcscf("P-CSCF: new UE connected — waiting for REGISTER to learn IMPU");

        auto* ses = new UeSession();
        ses->sock = std::move(ue_sock);
        {
            std::lock_guard<std::mutex> lk(ue_mtx_);
            ue_sessions_.emplace_back(ses);
        }
        ue_threads_.emplace_back([this, ses]{ ueReceiveLoop(ses); });
    }
}

void PcscfNode::ueReceiveLoop(UeSession* ses) {
    while (!stop_.load()) {
        if (!ses->sock.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!ses->sock.recvFrame(payload)) break;
        handleFromUe(ses, payload);
    }
}

void PcscfNode::scscfReceiveLoop() {
    while (!stop_.load()) {
        if (!scscf_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!scscf_conn_.recvFrame(payload)) break;
        handleFromScscf(payload);
    }
}

void PcscfNode::handleFromUe(UeSession* ses, const std::vector<uint8_t>& payload) {
    MessageReader r(payload);
    auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));

    // Extract key headers
    std::string from, to, impu, impi, contact, call_id, sdp;
    MessageReader r2(payload);
    while (r2.hasMore()) {
        Tag tag; uint16_t len; if (!r2.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM)))    from    = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_TO)))  to      = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_IMPU))) impu   = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_IMPI))) impi   = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CONTACT))) contact = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP)))  sdp    = r2.readStr();
        else r2.skip();
    }

    switch (type) {
    case SipMsgType::SIP_REGISTER: {
        Logger::pcscf("P-CSCF: ← SIP REGISTER from new UE  IMPU=" + impu);
        Logger::ie_field("  Contact: " + contact + "  (4G IP from P-GW!)");
        Logger::ie_field("  Via: added by P-CSCF for route tracing");
        // Register UE session
        {
            std::lock_guard<std::mutex> lk(ue_mtx_);
            ses->impu = impu; ses->registered = false;
            ue_by_impu_[impu] = ses;
        }
        Logger::pcscf("P-CSCF: → forwarding REGISTER to S-CSCF");
        sendToScscf(payload);
        break;
    }
    case SipMsgType::SIP_INVITE: {
        Logger::pcscf("P-CSCF: ← SIP INVITE from " + (from.empty()?ses->impu:from));
        Logger::ie_field("  To:      " + to);
        Logger::ie_field("  Call-ID: " + call_id + "  (unique dialog ID)");
        Logger::ie_field("  SDP:     " + (sdp.empty()? "audio/video offer" : sdp));
        // Store routing: call_id → caller
        {
            std::lock_guard<std::mutex> lk(call_mtx_);
            std::string caller = from.empty() ? ses->impu : from;
            call_to_caller_[call_id] = caller;
            call_to_callee_[call_id] = to;
        }
        Logger::pcscf("P-CSCF: → forwarding INVITE to S-CSCF (will invoke MTAS)");
        sendToScscf(payload);
        break;
    }
    case SipMsgType::SIP_200_OK: {
        // 200 OK from callee (accepting call) or re-INVITE response
        Logger::pcscf("P-CSCF: ← SIP 200 OK from " + ses->impu + "  Call-ID=" + call_id);
        Logger::ie_field("  SDP answer: codec=AMR-WB/16000 — HD Voice negotiated");
        Logger::ie_field("  To-tag: dialog fully established");
        sendToScscf(payload);
        // Trigger Rx AAR → QCI=1 bearer
        sendRxAAR(ses->impu, call_id);
        break;
    }
    case SipMsgType::SIP_ACK: {
        Logger::pcscf("P-CSCF: ← SIP ACK from " + ses->impu);
        Logger::ie_field("  Confirms 200 OK receipt — SIP 3-way handshake complete");
        sendToScscf(payload);
        break;
    }
    case SipMsgType::SIP_BYE: {
        Logger::pcscf("P-CSCF: ← SIP BYE from " + ses->impu + "  Call-ID=" + call_id);
        Logger::ie_field("  After 200 OK: Rx STR → PCRF → release QCI=1 bearer");
        sendToScscf(payload);
        break;
    }
    default: {
        Logger::pcscf("P-CSCF: ← " + std::string(sip_type_str(type)) + " from UE — forwarding");
        sendToScscf(payload);
        break;
    }
    }
}

void PcscfNode::handleFromScscf(const std::vector<uint8_t>& payload) {
    MessageReader r(payload);
    auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));

    // Extract routing headers
    std::string to, from, call_id, reason, sdp;
    MessageReader r2(payload);
    while (r2.hasMore()) {
        Tag tag; uint16_t len; if (!r2.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_TO)))       to      = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM))) from    = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_REASON))) reason = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP)))  sdp    = r2.readStr();
        else r2.skip();
    }

    switch (type) {
    case SipMsgType::SIP_INVITE: {
        // S-CSCF routing INVITE to callee — deliver to callee UE
        Logger::pcscf("P-CSCF: ← S-CSCF routing INVITE → delivering to callee " + to);
        Logger::ie_field("  MTAS approved: no barring, OIP OK, CDR started");
        Logger::ie_field("  Call-ID: " + call_id);
        sendToUe(to, payload);
        break;
    }
    case SipMsgType::SIP_100_TRYING: {
        Logger::pcscf("P-CSCF: ← 100 Trying → forwarding to caller");
        std::string caller;
        {
            std::lock_guard<std::mutex> lk(call_mtx_);
            auto it = call_to_caller_.find(call_id);
            if (it != call_to_caller_.end()) caller = it->second;
        }
        if (!caller.empty()) sendToUe(caller, payload);
        break;
    }
    case SipMsgType::SIP_180_RINGING: {
        Logger::pcscf("P-CSCF: ← 180 Ringing → forwarding to caller");
        Logger::ie_field("  To-tag added — SIP dialog established!");
        Logger::ie_field("  Caller hears ringback tone");
        std::string caller;
        {
            std::lock_guard<std::mutex> lk(call_mtx_);
            auto it = call_to_caller_.find(call_id);
            if (it != call_to_caller_.end()) caller = it->second;
        }
        if (!caller.empty()) sendToUe(caller, payload);
        break;
    }
    case SipMsgType::SIP_200_OK: {
        Logger::pcscf("P-CSCF: ← 200 OK (" + reason + ") → forwarding to caller");
        if (reason == "INVITE" || reason.empty()) {
            Logger::ie_field("  SDP answer: " + (sdp.empty() ? "AMR-WB/16000" : sdp));
            Logger::pcscf("P-CSCF: → Rx AAR to PCRF — requesting QCI=1 dedicated bearer");
            sendRxAAR("caller", call_id);
        }
        std::string caller;
        {
            std::lock_guard<std::mutex> lk(call_mtx_);
            auto it = call_to_caller_.find(call_id);
            if (it != call_to_caller_.end()) caller = it->second;
        }
        if (!caller.empty()) sendToUe(caller, payload);
        break;
    }
    default: {
        // Route to caller if we know the call_id, else broadcast
        std::string caller;
        {
            std::lock_guard<std::mutex> lk(call_mtx_);
            auto it = call_to_caller_.find(call_id);
            if (it != call_to_caller_.end()) caller = it->second;
        }
        if (!caller.empty()) sendToUe(caller, payload);
        else if (!to.empty()) sendToUe(to, payload);
        break;
    }
    }
}

void PcscfNode::sendToScscf(const std::vector<uint8_t>& frame) {
    if (scscf_conn_.valid()) scscf_conn_.sendFrame(frame);
}

void PcscfNode::sendToUe(const std::string& impu, const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lk(ue_mtx_);
    auto it = ue_by_impu_.find(impu);
    if (it != ue_by_impu_.end() && it->second->sock.valid())
        it->second->sock.sendFrame(frame);
    else
        Logger::warn("P-CSCF", "no socket for IMPU=" + impu + " — UE not registered?");
}

void PcscfNode::sendRxAAR(const std::string& impu, const std::string& call_id) {
    Logger::pcscf("P-CSCF: → Diameter Rx AAR to PCRF [TS 29.214]");
    Logger::ie_field("  User: " + impu + "  Call-ID: " + call_id);
    Logger::ie_field("  Media: AMR-WB 12.65kbps, direction=sendrecv");
    Logger::ie_field("  Result: PCRF→P-GW Gx RAR → QCI=1 dedicated bearer created");
    Logger::ie_field("  Voice now flows on QCI=1 (priority above all data)");
    PcapWriter::instance().writeDiameter(
        DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, true,
        PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA,
        PcapWriter::IP_PCRF, PcapWriter::PORT_GX);
    PcapWriter::instance().writeDiameter(
        DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, false,
        PcapWriter::IP_PCRF, PcapWriter::PORT_GX,
        PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA);
}
