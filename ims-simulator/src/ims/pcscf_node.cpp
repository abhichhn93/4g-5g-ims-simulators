#include "ims/pcscf_node.h"#include "common/logger.h"
#include "common/pcap_writer.h"
#include "common/visual_logger.h"
#include "ims/sip_text.h"
#include "ims/ims_diagrams.h"
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
    Logger::pcscf(Logger::Level::SYSTEM, "P-CSCF: starting — multi-UE SIP proxy on port " + std::to_string(PCSCF_PORT));
    try {
        connectToSCscf();
        server_socket_ = Socket::createServer(PCSCF_IP, PCSCF_PORT);
        
        // ATOMIC: Ready flag is set after all connections are stable
        pcscf_ready_.store(true);
        
        Logger::pcscf(Logger::Level::SYSTEM, "P-CSCF: ready ✓ — waiting for UE-A, UE-B, UE-C...");

        std::thread scscf_th([this]{ scscfReceiveLoop(); });
        acceptLoop();
        scscf_th.join();
        for (auto& t : ue_threads_) if (t.joinable()) t.join();
    } catch (const std::exception& e) {
        Logger::pcscf(Logger::Level::INTERVIEW_C, 
            "Senior Tip: Always catch by 'const std::exception&' to avoid slicing "
            "and ensure you catch all derived custom telecom exceptions.");
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
        Logger::sys("  " + label + " : REGISTERED  IMPU=" + impu + "  Contact=" + ses->ip);
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
    Logger::pcscf(Logger::Level::SYSTEM, "P-CSCF: connecting to S-CSCF on port " + std::to_string(SCSCF_PORT));
    for (int i = 0; i < 50 && !stop_.load(); ++i) {
        try { scscf_conn_ = Socket::connectTo(SCSCF_IP, SCSCF_PORT); break; }
        catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    Logger::pcscf(Logger::Level::SYSTEM, "P-CSCF: S-CSCF connected ✓");
}

void PcscfNode::acceptLoop() {
    while (!stop_.load()) {
        if (!server_socket_.hasConnection(200)) continue;
        Socket ue_sock = server_socket_.accept();
        
        // INTERVIEW_C: Memory Ownership
        // Using shared_ptr for sessions because the 'ueReceiveLoop' thread and 
        // the 'ue_sessions_' list share the ownership.
        // INTERVIEW: std::shared_ptr (Shared Memory Ownership)
        // The session object is shared between the main vector and the thread.
        // shared_ptr ensures it's deleted only after BOTH release it.
        auto ses = std::make_shared<UeSession>();
        ses->sock = std::move(ue_sock);
        
        { 
            std::lock_guard<std::mutex> lk(ue_mtx_); 
            ue_sessions_.push_back(ses); 
        }
        
        ue_threads_.emplace_back([this, ses]{ ueReceiveLoop(ses.get()); });
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
    Logger::pcscf(Logger::Level::SYSTEM, "P-CSCF: scscfReceiveLoop started — watching for S-CSCF responses");
    while (!stop_.load()) {
        if (!scscf_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!scscf_conn_.recvFrame(payload)) {
            Logger::warn("P-CSCF", "S-CSCF connection lost — attempting reconnect");
            // Try to reconnect to S-CSCF rather than dying
            for (int i = 0; i < 30 && !stop_.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                try { scscf_conn_ = Socket::connectTo(SCSCF_IP, SCSCF_PORT); break; }
                catch (...) {}
            }
            continue;
        }
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: [scscf_rx] received " + std::to_string(payload.size()) + " bytes from S-CSCF");
        handleFromScscf(payload);
    }
    Logger::pcscf(Logger::Level::SYSTEM, "P-CSCF: scscfReceiveLoop exiting");
}

// ── Helper: route to UE by IMPU ──────────────────────────────
static std::string ueLabel(const std::string& impu) {
    if (impu.find("+919000000001") != std::string::npos) return "UE-A";
    if (impu.find("+919000000002") != std::string::npos) return "UE-B";
    if (impu.find("+919000000003") != std::string::npos) return "UE-C";
    return impu;
}

// Map a UE's IMPU to its pcap IP — numeric (PcapWriter) and string (SipText) forms.
static uint32_t ueIp(const std::string& impu) {
    std::string label = ueLabel(impu);
    if (label == "UE-B") return PcapWriter::IP_UE_B;
    if (label == "UE-C") return PcapWriter::IP_UE_C;
    return PcapWriter::IP_UE;
}
static std::string ueIpStr(const std::string& impu) {
    std::string label = ueLabel(impu);
    if (label == "UE-B") return IP_UE_B;
    if (label == "UE-C") return IP_UE_C;
    return IP_UE_A;
}

// Diagram helpers — all delegated to ims_diagrams.h

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
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: ← SIP REGISTER from " + ueLabel(impu));
        Logger::ie_field("  IMPU:    " + impu);
        Logger::ie_field("  Contact: " + contact + "  (4G IP from EPC P-GW!)");
        Logger::ie_field("  P-Access-Network-Info: 3GPP-E-UTRAN-FDD");
        Diag::Registration(impu, contact);
        { std::lock_guard<std::mutex> lk(ue_mtx_); ses->impu = impu; ses->ip = contact; ue_by_impu_[impu] = ses; }
        sendToScscf(payload);
        // PCAP
        PcapWriter::instance().writeSIP(
            SipText::buildRegister(impu, contact, 1),
            PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_SCSCF, 5060);
        break;
    }
    case SipMsgType::SIP_INVITE: {
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: ← SIP INVITE from " + ueLabel(from.empty() ? ses->impu : from));
        Logger::ie_field("  To:      " + to + "  (" + ueLabel(to) + ")");
        Logger::ie_field("  Call-ID: " + call_id);
        Logger::ie_field("  SDP:     " + (sdp.empty() ? "audio/AMR-WB + video/H264" : sdp));
        std::string caller_impu = from.empty() ? ses->impu : from;
        {
            std::lock_guard<std::mutex> lk(call_mtx_);
            call_to_caller_[call_id] = caller_impu;
            call_to_callee_[call_id] = to;
        }
        Diag::CallSetup(caller_impu, to);
        sendToScscf(payload);
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: added hop headers before forwarding to S-CSCF →");
        Logger::ie_field("  + Via: SIP/2.0/TCP 10.0.0.8:5060;branch=z9hG4bK-pcscf  ← P-CSCF stamps itself");
        Logger::ie_field("  + Route: <sip:scscf.ims...;lr>  ← service route to S-CSCF");
        Logger::ie_field("  + P-Visited-Network-ID: mnc010.mcc404.3gppnetwork.org");
        Logger::ie_field("  Unchanged: From, To, Call-ID, CSeq, SDP");
        Logger::pcscf(Logger::Level::BEGINNER, "P-CSCF stamped its address on the INVITE — replies will come back through here");
        PcapWriter::instance().writeSIP(
            SipText::buildInvite(caller_impu, to, "10.0.0.x", call_id, 1),
            PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_SCSCF, 5060);
        break;
    }
    case SipMsgType::SIP_200_OK: {
        // 200 OK from callee — forward to S-CSCF which routes to caller
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: ← SIP 200 OK from " + ueLabel(ses->impu) + "  (accepting call)");
        Logger::ie_field("  SDP answer: AMR-WB/16000 — HD Voice codec negotiated");
        Logger::ie_field("  To-tag: dialog established");
        sendToScscf(payload);
        sendRxAAR(ses->impu, call_id);
        break;
    }
    case SipMsgType::SIP_ACK:
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: ← ACK from " + ueLabel(ses->impu) + "  forwarding to callee");
        sendToScscf(payload);
        break;
    case SipMsgType::SIP_BYE: {
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: ← BYE from " + ueLabel(ses->impu) + "  Call-ID=" + call_id);
        Logger::ie_field("  Rx STR → PCRF → QCI=1 bearer will be released");
        // Find peer for BYE diagram
        std::string peer;
        { std::lock_guard<std::mutex> lk2(call_mtx_);
          auto it = call_to_callee_.find(call_id);
          if (it != call_to_callee_.end()) peer = it->second;
          else { auto it2 = call_to_caller_.find(call_id);
                 if (it2 != call_to_caller_.end()) peer = it2->second; } }
        Diag::CallEnd(ses->impu, peer);
        sendToScscf(payload);
        // Clean up routing
        { std::lock_guard<std::mutex> lk2(call_mtx_);
          call_to_caller_.erase(call_id);
          call_to_callee_.erase(call_id);
          call_invite_delivered_.erase(call_id); }
        break;
    }
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
        // Distinguish initial INVITE from re-INVITE (HOLD/VIDEO/RESUME/conf sub-call)
        // using a dedicated delivered-set — call_to_callee_ is populated by handleFromUe
        // on every INVITE (including re-INVITEs), so it can't be used as the guard.
        bool is_reinvite;
        { std::lock_guard<std::mutex> lk(call_mtx_);
          is_reinvite = (call_invite_delivered_.count(call_id) > 0);
          if (!is_reinvite) {
              call_invite_delivered_.insert(call_id);
              // Conference sub-calls (call_id+"-conf") arrive from S-CSCF without prior
              // registration from handleFromUe — register them now so 200 OK can route back.
              if (call_to_caller_.find(call_id) == call_to_caller_.end() && !from.empty())
                  call_to_caller_[call_id] = from;
              if (call_to_callee_.find(call_id) == call_to_callee_.end() && !to.empty())
                  call_to_callee_[call_id] = to;
          }
        }

        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: routing " +
            std::string(is_reinvite ? "re-INVITE" : "INVITE") + " → " + ueLabel(to));

        if (!is_reinvite) {
            // IMS-2 Hop 3 — routing header diff (only meaningful for new INVITE)
            Logger::ie_field("  MTAS checked: OIP OK, no barring");
            Logger::pcscf(Logger::Level::BEGINNER,
                ueLabel(to) + "'s phone is ringing — incoming call from " + ueLabel(from));
            Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: stripped routing headers before delivering to UE →");
            Logger::ie_field("  - Route headers consumed  (each proxy pops its own Route)");
            Logger::ie_field("  - Via: SIP/2.0/TCP 10.0.0.9:5070 removed  (S-CSCF Via popped)");
            Logger::ie_field("  Kept: From, To, Call-ID, SDP, P-Asserted-Identity");
            Logger::pcscf(Logger::Level::BEGINNER,
                "Routing labels stripped — only the call details arrive at " + ueLabel(to) + "'s phone");

            // IMS-3 — call waiting detection
            bool callee_busy = false;
            { std::lock_guard<std::mutex> lk(call_mtx_);
              for (auto& [cid, cee] : call_to_callee_)
                if (cee == to && cid != call_id) { callee_busy = true; break; }
            }
            if (callee_busy) {
                Logger::pcscf(Logger::Level::ENGINEER,
                    "P-CSCF: MTAS → Call Waiting! " + ueLabel(to) + " is in a call — ringing second line");
                Logger::ie_field("  CW service (MMTEL TS 24.173 §7.6): INVITE still delivered to callee");
                Logger::ie_field("  Callee sees call-waiting indicator; can ACCEPT (hold current) or REJECT");
                Logger::pcscf(Logger::Level::BEGINNER,
                    ueLabel(to) + " is already on a call — their phone rings a second time (call waiting)");
            }
        } else {
            Logger::pcscf(Logger::Level::BEGINNER, ueLabel(to) + "'s call is being modified");
        }

        sendToUe(to, payload);

        if (!is_reinvite) {
            // SIM: P-CSCF synthesizes 180 Ringing the moment the INVITE is
            // successfully delivered to the callee's terminal — a documented
            // simplification vs. real IMS, where 180 Ringing originates from
            // the callee's own UE stack. Still causally real: it only fires
            // after delivery succeeds.
            MessageWriter ring(static_cast<MessageType>(uint16_t(SipMsgType::SIP_180_RINGING)), next_seq_++);
            ring.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), call_id);
            ring.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    from);
            ring.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      to);
            ring.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_REASON)),  "INVITE");
            Logger::pcscf(Logger::Level::ENGINEER,
                "P-CSCF: → 180 Ringing → " + ueLabel(from) + "  (" + ueLabel(to) + " alerting)");
            Logger::pcscf(Logger::Level::BEGINNER,
                ueLabel(from) + " hears ringback — " + ueLabel(to) + "'s phone is ringing");
            sendToUe(from, ring.udpPayload());
            PcapWriter::instance().writeSIP(
                SipText::build180Ringing(from, to, call_id, 1),
                PcapWriter::IP_PCSCF, 5060, ueIp(from), 5060);
        }
        break;
    }

    case SipMsgType::SIP_100_TRYING: {
        // Route to caller
        std::string caller;
        { std::lock_guard<std::mutex> lk(call_mtx_);
          auto it = call_to_caller_.find(call_id);
          if (it != call_to_caller_.end()) caller = it->second; }
        if (caller.empty()) caller = from;
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: 100 Trying → " + ueLabel(caller));
        if (!caller.empty()) sendToUe(caller, payload);
        break;
    }

    case SipMsgType::SIP_180_RINGING: {
        std::string caller;
        { std::lock_guard<std::mutex> lk(call_mtx_);
          auto it = call_to_caller_.find(call_id);
          if (it != call_to_caller_.end()) caller = it->second; }
        if (caller.empty()) caller = from;
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: 180 Ringing → " + ueLabel(caller));
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
            Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: 200 OK (REGISTER) → " + ueLabel(target) + " ✓");
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

            Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: 200 OK (" + reason + ") → " + ueLabel(target));

            if (reason == "INVITE" || reason.empty()) {
                std::string callee;
                { std::lock_guard<std::mutex> lk2(call_mtx_);
                  auto it = call_to_callee_.find(call_id);
                  if (it != call_to_callee_.end()) callee = it->second; }
                Logger::ie_field("  SDP answer: " + (sdp.empty() ? "AMR-WB/16000 HD Voice" : sdp));
                Logger::pcscf(Logger::Level::BEGINNER, ueLabel(callee) + " answered — call connected!");
                Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: → Rx AAR to PCRF — QCI=1 dedicated bearer!");
                sendRxAAR(target, call_id);
                PcapWriter::instance().writeSIP(
                    SipText::build200Invite(callee, target, ueIpStr(callee), call_id, 1),
                    PcapWriter::IP_SCSCF, 5060, ueIp(target), 5060);
            } else if (reason == "CONFERENCE") {
                Logger::pcscf(Logger::Level::BEGINNER, "3-way conference is live — all 3 can talk now");
                Logger::pcscf(Logger::Level::ENGINEER,
                    "P-CSCF: conference 200 OK → " + ueLabel(target) + "  (MRFC bridge active)");
                PcapWriter::instance().writeSIP(
                    SipText::build200Invite(to, target, ueIpStr(to), call_id, 1),
                    PcapWriter::IP_MRFC, 5060, ueIp(target), 5060);
            } else if (reason == "re-INVITE-HOLD") {
                std::string callee;
                { std::lock_guard<std::mutex> lk2(call_mtx_);
                  auto it = call_to_callee_.find(call_id);
                  if (it != call_to_callee_.end()) callee = it->second; }
                Diag::Hold(target, callee);
                Logger::pcscf(Logger::Level::BEGINNER, ueLabel(target) + " put the call on hold");
                Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: HOLD 200 OK → " + ueLabel(target) + " + notifying " + ueLabel(callee));
                PcapWriter::instance().writeSIP(
                    SipText::buildReInvite(target, callee, ueIpStr(target), call_id, 1,
                        "v=0\r\no=ue 1 1 IN IP4 " + ueIpStr(target) + "\r\ns=-\r\nt=0 0\r\n"
                        "m=audio 50000 RTP/AVP 98\r\na=rtpmap:98 AMR-WB/16000\r\na=sendonly\r\n"),
                    PcapWriter::IP_SCSCF, 5060, ueIp(callee), 5060);
                if (!callee.empty()) sendToUe(callee, payload);
            } else if (reason == "re-INVITE-RESUME") {
                std::string callee;
                { std::lock_guard<std::mutex> lk2(call_mtx_);
                  auto it = call_to_callee_.find(call_id);
                  if (it != call_to_callee_.end()) callee = it->second; }
                Diag::Resume(target, callee);
                Logger::pcscf(Logger::Level::BEGINNER, "Call resumed — both sides talking again");
                Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: RESUME 200 OK → " + ueLabel(target) + " + notifying " + ueLabel(callee));
                PcapWriter::instance().writeSIP(
                    SipText::buildReInvite(target, callee, ueIpStr(target), call_id, 1,
                        "v=0\r\no=ue 1 1 IN IP4 " + ueIpStr(target) + "\r\ns=-\r\nt=0 0\r\n"
                        "m=audio 50000 RTP/AVP 98\r\na=rtpmap:98 AMR-WB/16000\r\na=sendrecv\r\n"),
                    PcapWriter::IP_SCSCF, 5060, ueIp(callee), 5060);
                if (!callee.empty()) sendToUe(callee, payload);
            } else if (reason == "VIDEO-ADD") {
                std::string callee;
                { std::lock_guard<std::mutex> lk2(call_mtx_);
                  auto it = call_to_callee_.find(call_id);
                  if (it != call_to_callee_.end()) callee = it->second; }
                Logger::pcscf(Logger::Level::BEGINNER, "Video is now active — both sides on video call");
                Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: VIDEO-ADD 200 OK → " + ueLabel(target) + " + notifying " + ueLabel(callee));
                PcapWriter::instance().writeSIP(
                    SipText::buildReInvite(target, callee, ueIpStr(target), call_id, 1,
                        "v=0\r\no=ue 1 1 IN IP4 " + ueIpStr(target) + "\r\ns=-\r\nt=0 0\r\n"
                        "m=audio 50000 RTP/AVP 98\r\na=rtpmap:98 AMR-WB/16000\r\na=sendrecv\r\n"
                        "m=video 50002 RTP/AVP 100\r\na=rtpmap:100 H264/90000\r\na=sendrecv\r\n"),
                    PcapWriter::IP_SCSCF, 5060, ueIp(callee), 5060);
                if (!callee.empty()) sendToUe(callee, payload);
            } else if (reason == "VIDEO-REMOVE") {
                std::string callee;
                { std::lock_guard<std::mutex> lk2(call_mtx_);
                  auto it = call_to_callee_.find(call_id);
                  if (it != call_to_callee_.end()) callee = it->second; }
                Logger::pcscf(Logger::Level::BEGINNER, "Video dropped — voice-only call active");
                Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: VIDEO-REMOVE 200 OK → " + ueLabel(target) + " + notifying " + ueLabel(callee));
                PcapWriter::instance().writeSIP(
                    SipText::buildReInvite(target, callee, ueIpStr(target), call_id, 1,
                        "v=0\r\no=ue 1 1 IN IP4 " + ueIpStr(target) + "\r\ns=-\r\nt=0 0\r\n"
                        "m=audio 50000 RTP/AVP 98\r\na=rtpmap:98 AMR-WB/16000\r\na=sendrecv\r\n"
                        "m=video 0 RTP/AVP 100\r\na=rtpmap:100 H264/90000\r\n"),
                    PcapWriter::IP_SCSCF, 5060, ueIp(callee), 5060);
                if (!callee.empty()) sendToUe(callee, payload);
            }
        }

        if (!target.empty()) sendToUe(target, payload);
        break;
    }

    case SipMsgType::SIP_ACK: {
        // ACK travels caller → callee (the opposite direction of every other
        // response above) — route via call_to_callee_, not call_to_caller_.
        std::string callee;
        { std::lock_guard<std::mutex> lk(call_mtx_);
          auto it = call_to_callee_.find(call_id);
          if (it != call_to_callee_.end()) callee = it->second; }
        if (callee.empty()) callee = to;
        Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: ACK → " + ueLabel(callee) + "  (3-way handshake complete)");
        Logger::pcscf(Logger::Level::BEGINNER, "Call fully set up — both sides can talk now");
        if (!callee.empty()) {
            sendToUe(callee, payload);
            PcapWriter::instance().writeSIP(
                SipText::buildAck(from, callee, call_id, 1),
                PcapWriter::IP_PCSCF, 5060, ueIp(callee), 5060);

            // ── End-of-call summary — content depends on LOG_LEVEL ──────
            if (Logger::getGlobalLevel() == Logger::Level::BEGINNER) {
                VLog::step(1, 1, "CALL ESTABLISHED — Summary",
                           ueLabel(from), Logger::CLR_ENB, ueLabel(callee), Logger::CLR_ENB)
                    .ie("What happened", ueLabel(from) + " called " + ueLabel(callee) + ". " +
                                          ueLabel(callee) + "'s phone rang, " + ueLabel(callee) + " answered.")
                    .ie("Result",        "Both phones now show 'in call' — voice would flow over RTP.")
                    .next("Type BYE on either terminal to end the call.")
                    .flush();
            } else {
                VLog::step(1, 1, "CALL ESTABLISHED — SIP 3-way handshake complete",
                           ueLabel(from), Logger::CLR_ENB, ueLabel(callee), Logger::CLR_ENB)
                    .ie("INVITE",      "Call-ID=" + call_id + "  From=" + from + " To=" + callee)
                    .ie("180 Ringing", "To-tag added — dialog established")
                    .ie("200 OK",      "SDP answer negotiated (AMR-WB/16000)")
                    .ie("ACK",         "3-way handshake complete — dialog confirmed")
                    .next("QCI=1 bearer requested via Rx AAR (see P-CSCF logs above)")
                    .flush();
            }
        }
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

void PcscfNode::sendToScscf(const std::vector<uint8_t>& payload) {
    // payload came from recvFrame (NO length prefix) → use sendPayload to add it back
    if (!scscf_conn_.valid()) {
        Logger::warn("P-CSCF", "sendToScscf: connection invalid");
        return;
    }
    bool ok = scscf_conn_.sendPayload(payload);
    if (!ok) Logger::warn("P-CSCF", "sendToScscf FAILED — connection broken");
}

void PcscfNode::sendToUe(const std::string& impu, const std::vector<uint8_t>& payload) {
    // payload came from recvFrame (NO length prefix) → use sendPayload to add it back
    std::lock_guard<std::mutex> lk(ue_mtx_);
    auto it = ue_by_impu_.find(impu);
    if (it != ue_by_impu_.end() && it->second->sock.valid()) {
        bool ok = it->second->sock.sendPayload(payload);
        if (ok)
            Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: → delivered to " + ueLabel(impu) + " ✓");
        else
            Logger::warn("P-CSCF", "sendPayload FAILED for " + ueLabel(impu));
    } else {
        Logger::warn("P-CSCF", "NO ROUTE to " + ueLabel(impu) +
                     " (known: " + std::to_string(ue_by_impu_.size()) + " UEs)");
    }
}

void PcscfNode::sendRxAAR(const std::string& impu, const std::string& call_id) {
    Logger::pcscf(Logger::Level::ENGINEER, "P-CSCF: → Diameter Rx AAR to PCRF [TS 29.214]");
    Logger::ie_field("  User: " + ueLabel(impu) + "  Call-ID: " + call_id);
    Logger::ie_field("  Media: AMR-WB 12.65kbps, dir=sendrecv");
    Logger::ie_field("  Result: PCRF→P-GW→MME→eNB: QCI=1 DRB created!");
    PcapWriter::instance().writeDiameter(DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, true,
        PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA, PcapWriter::IP_PCRF, PcapWriter::PORT_GX);
    PcapWriter::instance().writeDiameter(DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, false,
        PcapWriter::IP_PCRF, PcapWriter::PORT_GX, PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA);
}
