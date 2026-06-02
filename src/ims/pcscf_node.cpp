#include "ims/pcscf_node.h"
#include "common/logger.h"
#include "common/pcap_writer.h"
#include "common/visual_logger.h"
#include "ims/sip_text.h"
#include <chrono>
#include <thread>
#include <stdexcept>
#include <sstream>

static constexpr const char* PCSCF_IP  = "127.0.0.1";
static constexpr uint16_t    PCSCF_PORT = 5060;
static constexpr const char* SCSCF_IP  = "127.0.0.1";
static constexpr uint16_t    SCSCF_PORT = 5070;

PcscfNode::PcscfNode(std::atomic<bool>& stop, std::atomic<bool>& pcscf_ready)
    : stop_(stop), pcscf_ready_(pcscf_ready) {}

void PcscfNode::run() {
    Logger::pcscf("P-CSCF: starting — multi-UE SIP proxy on port " + std::to_string(PCSCF_PORT));
    try {
        connectToSCscf();
        server_socket_ = Socket::createServer(PCSCF_IP, PCSCF_PORT);
        pcscf_ready_.store(true);
        Logger::pcscf("P-CSCF: ready ✓ — waiting for UE-A, UE-B, UE-C...");

        std::thread scscf_th([this]{ scscfReceiveLoop(); });
        acceptLoop();
        scscf_th.join();
        for (auto& t : ue_threads_) if (t.joinable()) t.join();
    } catch (const std::exception& e) {
        Logger::warn("P-CSCF", e.what());
    }
}

void PcscfNode::printStatus() {
    std::lock_guard<std::mutex> lk(ue_mtx_);
    Logger::sys("=== IMS SERVER STATUS ===");
    Logger::sys("  Connected UEs: " + std::to_string(ue_sessions_.size()));
    for (auto& [impu, ses] : ue_by_impu_) {
        std::string label = impu.find("+919000000001") != std::string::npos ? "UE-A" :
                            impu.find("+919000000002") != std::string::npos ? "UE-B" : "UE-C";
        Logger::sys("  " + label + " : REGISTERED  IMPU=" + impu);
    }
    {
        std::lock_guard<std::mutex> lk2(call_mtx_);
        if (call_to_caller_.empty()) {
            Logger::sys("  Active calls: none");
        } else {
            Logger::sys("  Active calls: " + std::to_string(call_to_caller_.size()));
            for (auto& [cid, caller] : call_to_caller_) {
                auto it = call_to_callee_.find(cid);
                std::string callee = (it != call_to_callee_.end()) ? it->second : "?";
                Logger::sys("    Call-ID: " + cid);
                Logger::sys("      Caller: " + caller);
                Logger::sys("      Callee: " + callee);
            }
        }
    }
    Logger::sys("=========================");
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
        { std::lock_guard<std::mutex> lk(ue_mtx_); ue_sessions_.emplace_back(ses); }
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

// ── Helper: route to UE by IMPU ──────────────────────────────
static std::string ueLabel(const std::string& impu) {
    if (impu.find("+919000000001") != std::string::npos) return "UE-A";
    if (impu.find("+919000000002") != std::string::npos) return "UE-B";
    if (impu.find("+919000000003") != std::string::npos) return "UE-C";
    return impu;
}

// ── Print registration flow diagram ──────────────────────────
static void printRegDiagram(const std::string& impu, const std::string& contact) {
    std::string label = ueLabel(impu);
    std::lock_guard<std::mutex> lk(Logger::getMutex());
    std::cout << Logger::CLR_PCSCF
        << "\n  ══ REGISTRATION FLOW: " << label << " ══════════════════════════════\n"
        << "\n"
        << "  " << label << "(" << contact << ")  P-CSCF    S-CSCF    IMS-HSS   MTAS\n"
        << "       │               │           │           │           │\n"
        << "       │──REGISTER────►│           │           │           │\n"
        << "       │  IMPU,Contact │──REGISTER►│           │           │\n"
        << "       │               │           │──Cx SAR──►│           │\n"
        << "       │               │           │  SAR: I serve this UE │\n"
        << "       │               │           │◄──Cx SAA──┤           │\n"
        << "       │               │           │  SAA: iFC + profile   │\n"
        << "       │               │           │──ISC REGISTER────────►│\n"
        << "       │               │           │  3rd-party reg        │\n"
        << "       │◄──200 OK──────┤◄──200 OK──┤           │           │\n"
        << "       │  P-Assoc-URI  │  Srv-Route│           │           │\n"
        << "  [REGISTERED]         │           │           │           │\n"
        << "  ════════════════════════════════════════════════════════\n"
        << Logger::CLR_RESET << "\n";
}

// ── Print call flow diagram ───────────────────────────────────
static void printCallDiagram(const std::string& caller_impu, const std::string& callee_impu) {
    std::string a = ueLabel(caller_impu);
    std::string b = ueLabel(callee_impu);
    std::lock_guard<std::mutex> lk(Logger::getMutex());
    std::cout << Logger::CLR_SCSCF
        << "\n  ══ VoLTE CALL FLOW: " << a << " → " << b << " ══════════════════════\n"
        << "\n"
        << "  " << a << "     P-CSCF    S-CSCF     MTAS      P-CSCF    " << b << "\n"
        << "   │          │           │           │           │           │\n"
        << "   │─INVITE──►│─INVITE───►│─ISC INV──►│           │           │\n"
        << "   │  SDP offer│           │ OIP+CDR   │           │           │\n"
        << "   │          │           │◄─continue─┤           │           │\n"
        << "   │          │           │──────────────────────►│─INVITE───►│\n"
        << "   │◄─100─────┤◄─100──────┤           │           │           │\n"
        << "   │          │           │           │           │◄─180 Ring─┤\n"
        << "   │◄─180─────┤◄─180──────┤           │           │           │\n"
        << "   │ (ringback)│           │           │           │◄─200 OK───┤\n"
        << "   │          │           │           │           │  SDP answr │\n"
        << "   │◄─200 OK──┤◄─200 OK───┤           │           │           │\n"
        << "   │ SDP answr │─Rx AAR───►PCRF        │           │           │\n"
        << "   │─ACK──────►│─ACK──────►│           │           │─ACK──────►│\n"
        << "   │          │QCI=1 bearer│           │           │           │\n"
        << "   │◄══════RTP/AMR-WB (QCI=1 dedicated bearer)════════════════►│\n"
        << "  ════════════════════════════════════════════════════════\n"
        << Logger::CLR_RESET << "\n";
}

// ── Print conference diagram ──────────────────────────────────
static void printConfDiagram() {
    std::lock_guard<std::mutex> lk(Logger::getMutex());
    std::cout << Logger::CLR_MTAS
        << "\n  ══ CONFERENCE CALL FLOW ══════════════════════════════════\n"
        << "\n"
        << "  UE-A     S-CSCF    MTAS     MRFC      MRFP    UE-B  UE-C\n"
        << "   │          │         │         │         │       │     │\n"
        << "   │─re-INV──►│─ISC────►│         │         │       │     │\n"
        << "   │          │         │─Mr INV──►│         │       │     │\n"
        << "   │          │         │  create  │─H.248──►│       │     │\n"
        << "   │          │         │  bridge  │ allocate│       │     │\n"
        << "   │          │         │◄─200 OK──┤ mixer   │       │     │\n"
        << "   │          │         │  conf-URI│         │       │     │\n"
        << "   │          │◄────────┤─INVITE──────────────────────────►│\n"
        << "   │          │         │          │         │       │◄INV─┤\n"
        << "   │◄─200 OK──┤         │          │         │       │     │\n"
        << "   │─RTP──────────────────────────►│◄─RTP───────────────── │\n"
        << "   │◄─mixed RTP (A hears B+C)──────┤                       │\n"
        << "   │  MRFC=controller   MRFP=DSP mixer (H.248/Megaco:2944) │\n"
        << "  ════════════════════════════════════════════════════════\n"
        << Logger::CLR_RESET << "\n";
}

void PcscfNode::handleFromUe(UeSession* ses, const std::vector<uint8_t>& payload) {
    MessageReader r(payload);
    auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));

    std::string from, to, impu, impi, contact, call_id, sdp;
    MessageReader r2(payload);
    while (r2.hasMore()) {
        Tag tag; uint16_t len; if (!r2.peek(tag, len)) break;
        if      (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM)))    from    = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_TO)))      to      = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_IMPU)))    impu    = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_IMPI)))    impi    = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CONTACT))) contact = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP)))     sdp     = r2.readStr();
        else r2.skip();
    }

    switch (type) {
    case SipMsgType::SIP_REGISTER: {
        Logger::pcscf("P-CSCF: ← SIP REGISTER from " + ueLabel(impu));
        Logger::ie_field("  IMPU:    " + impu);
        Logger::ie_field("  Contact: " + contact + "  (4G IP from EPC P-GW!)");
        Logger::ie_field("  P-Access-Network-Info: 3GPP-E-UTRAN-FDD");
        printRegDiagram(impu, contact);
        { std::lock_guard<std::mutex> lk(ue_mtx_); ses->impu = impu; ue_by_impu_[impu] = ses; }
        sendToScscf(payload);
        // PCAP
        PcapWriter::instance().writeSIP(
            SipText::buildRegister(impu, contact, 1),
            PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_SCSCF, 5060);
        break;
    }
    case SipMsgType::SIP_INVITE: {
        Logger::pcscf("P-CSCF: ← SIP INVITE from " + ueLabel(from.empty() ? ses->impu : from));
        Logger::ie_field("  To:      " + to + "  (" + ueLabel(to) + ")");
        Logger::ie_field("  Call-ID: " + call_id);
        Logger::ie_field("  SDP:     " + (sdp.empty() ? "audio/AMR-WB + video/H264" : sdp));
        std::string caller_impu = from.empty() ? ses->impu : from;
        {
            std::lock_guard<std::mutex> lk(call_mtx_);
            call_to_caller_[call_id] = caller_impu;
            call_to_callee_[call_id] = to;
        }
        printCallDiagram(caller_impu, to);
        sendToScscf(payload);
        PcapWriter::instance().writeSIP(
            SipText::buildInvite(caller_impu, to, "10.0.0.x", call_id, 1),
            PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_SCSCF, 5060);
        break;
    }
    case SipMsgType::SIP_200_OK: {
        // 200 OK from callee — forward to S-CSCF which routes to caller
        Logger::pcscf("P-CSCF: ← SIP 200 OK from " + ueLabel(ses->impu) + "  (accepting call)");
        Logger::ie_field("  SDP answer: AMR-WB/16000 — HD Voice codec negotiated");
        Logger::ie_field("  To-tag: dialog established");
        sendToScscf(payload);
        sendRxAAR(ses->impu, call_id);
        break;
    }
    case SipMsgType::SIP_ACK:
        Logger::pcscf("P-CSCF: ← ACK from " + ueLabel(ses->impu) + "  forwarding to callee");
        sendToScscf(payload);
        break;
    case SipMsgType::SIP_BYE:
        Logger::pcscf("P-CSCF: ← BYE from " + ueLabel(ses->impu) + "  Call-ID=" + call_id);
        Logger::ie_field("  Rx STR → PCRF → QCI=1 bearer will be released");
        sendToScscf(payload);
        break;
    default:
        sendToScscf(payload);
        break;
    }
}

void PcscfNode::handleFromScscf(const std::vector<uint8_t>& payload) {
    MessageReader r(payload);
    auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));

    std::string to, from, call_id, reason, sdp;
    MessageReader r2(payload);
    while (r2.hasMore()) {
        Tag tag; uint16_t len; if (!r2.peek(tag, len)) break;
        if      (tag == static_cast<Tag>(uint16_t(SipTag::SIP_TO)))      to      = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM)))    from    = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_REASON)))  reason  = r2.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP)))     sdp     = r2.readStr();
        else r2.skip();
    }

    switch (type) {

    case SipMsgType::SIP_INVITE: {
        // S-CSCF delivering INVITE to callee UE
        Logger::pcscf("P-CSCF: routing INVITE → " + ueLabel(to));
        Logger::ie_field("  MTAS checked: OIP OK, no barring");
        sendToUe(to, payload);
        break;
    }

    case SipMsgType::SIP_100_TRYING: {
        // Route to caller
        std::string caller;
        { std::lock_guard<std::mutex> lk(call_mtx_);
          auto it = call_to_caller_.find(call_id);
          if (it != call_to_caller_.end()) caller = it->second; }
        if (caller.empty()) caller = from;
        Logger::pcscf("P-CSCF: 100 Trying → " + ueLabel(caller));
        if (!caller.empty()) sendToUe(caller, payload);
        break;
    }

    case SipMsgType::SIP_180_RINGING: {
        std::string caller;
        { std::lock_guard<std::mutex> lk(call_mtx_);
          auto it = call_to_caller_.find(call_id);
          if (it != call_to_caller_.end()) caller = it->second; }
        if (caller.empty()) caller = from;
        Logger::pcscf("P-CSCF: 180 Ringing → " + ueLabel(caller));
        Logger::ie_field("  To-tag added — SIP dialog established");
        if (!caller.empty()) sendToUe(caller, payload);
        break;
    }

    case SipMsgType::SIP_200_OK: {
        // ── THE BUG FIX ──────────────────────────────────────
        // For REGISTER 200 OK: reason="REGISTER", from=to=IMPU
        //   → route to the registering UE by IMPU
        // For INVITE 200 OK: call_id in call_to_caller_
        //   → route to caller

        std::string target;
        if (reason == "REGISTER") {
            // Registration complete — deliver 200 OK to the registering UE
            target = from.empty() ? to : from;  // IMPU of registering UE
            Logger::pcscf("P-CSCF: 200 OK (REGISTER) → " + ueLabel(target) + " ✓");
            Logger::ie_field("  P-Associated-URI, Service-Route delivered");
            Logger::ie_field("  UE is now REGISTERED in IMS");

            // Mark UE as registered in our local map
            { std::lock_guard<std::mutex> lk(ue_mtx_);
              auto it = ue_by_impu_.find(target);
              if (it != ue_by_impu_.end()) it->second->registered = true; }

            PcapWriter::instance().writeSIP(
                SipText::build200Register(target, "10.0.0.x", 1),
                PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);

        } else {
            // INVITE/BYE/re-INVITE 200 OK — route to caller
            { std::lock_guard<std::mutex> lk(call_mtx_);
              auto it = call_to_caller_.find(call_id);
              if (it != call_to_caller_.end()) target = it->second; }
            if (target.empty()) target = from;

            Logger::pcscf("P-CSCF: 200 OK (" + reason + ") → " + ueLabel(target));

            if (reason == "INVITE" || reason.empty()) {
                Logger::ie_field("  SDP answer: " + (sdp.empty() ? "AMR-WB/16000 HD Voice" : sdp));
                Logger::pcscf("P-CSCF: → Rx AAR to PCRF — QCI=1 dedicated bearer!");
                sendRxAAR(target, call_id);
            } else if (reason == "CONFERENCE") {
                printConfDiagram();
            }
        }

        if (!target.empty()) sendToUe(target, payload);
        break;
    }

    default: {
        std::string target;
        { std::lock_guard<std::mutex> lk(call_mtx_);
          auto it = call_to_caller_.find(call_id);
          if (it != call_to_caller_.end()) target = it->second; }
        if (target.empty()) target = from.empty() ? to : from;
        if (!target.empty()) sendToUe(target, payload);
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
        Logger::warn("P-CSCF", "no route to " + ueLabel(impu) + " — IMPU=" + impu);
}

void PcscfNode::sendRxAAR(const std::string& impu, const std::string& call_id) {
    Logger::pcscf("P-CSCF: → Diameter Rx AAR to PCRF [TS 29.214]");
    Logger::ie_field("  User: " + ueLabel(impu) + "  Call-ID: " + call_id);
    Logger::ie_field("  Media: AMR-WB 12.65kbps, dir=sendrecv");
    Logger::ie_field("  Result: PCRF→P-GW→MME→eNB: QCI=1 DRB created!");
    PcapWriter::instance().writeDiameter(DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, true,
        PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA, PcapWriter::IP_PCRF, PcapWriter::PORT_GX);
    PcapWriter::instance().writeDiameter(DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, false,
        PcapWriter::IP_PCRF, PcapWriter::PORT_GX, PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA);
}
