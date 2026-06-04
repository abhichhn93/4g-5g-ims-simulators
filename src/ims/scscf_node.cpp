#include "ims/scscf_node.h"
#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/pcap_writer.h"
#include "ims/ims_diagrams.h"
#include <chrono>
#include <thread>
#include <stdexcept>

static constexpr const char* SCSCF_IP   = "127.0.0.1";
static constexpr uint16_t    SCSCF_PORT  = 5070;
static constexpr const char* HSS_CX_IP  = "127.0.0.1";
static constexpr uint16_t    HSS_CX_PORT = 3870;

ScscfNode::ScscfNode(std::atomic<bool>& stop, std::atomic<bool>& scscf_ready,
                     std::atomic<bool>& hss_ready)
    : stop_(stop), scscf_ready_(scscf_ready), hss_ready_(hss_ready) {}

void ScscfNode::run() {
    Logger::scscf("S-CSCF: starting — SIP registrar + MTAS gateway");
    try { setupServer(); if (!stop_.load()) receiveLoop(); }
    catch (const std::exception& e) { Logger::warn("S-CSCF", e.what()); }
    Logger::scscf("S-CSCF: thread exiting");
}

void ScscfNode::setupServer() {
    server_socket_ = Socket::createServer(SCSCF_IP, SCSCF_PORT);
    for (int i = 0; i < 50 && !stop_.load(); ++i) {
        try { hss_conn_ = Socket::connectTo(HSS_CX_IP, HSS_CX_PORT); break; }
        catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    Logger::scscf("S-CSCF: IMS-HSS Cx link UP ✓");
    scscf_ready_.store(true);
    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            pcscf_conn_ = server_socket_.accept();
            Logger::scscf("S-CSCF: P-CSCF connected ✓");
            return;
        }
    }
}

void ScscfNode::receiveLoop() {
    Logger::scscf("S-CSCF: receive loop started");
    while (!stop_.load()) {
        if (!pcscf_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!pcscf_conn_.recvFrame(payload)) break;
        MessageReader r(payload);
        auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));
        switch (type) {
            case SipMsgType::SIP_REGISTER: handleRegister(payload); break;
            case SipMsgType::SIP_INVITE:   handleInvite(payload);   break;
            case SipMsgType::SIP_ACK:      handleAck(payload);      break;
            case SipMsgType::SIP_200_OK:   handle200Ok(payload);    break;
            case SipMsgType::SIP_BYE:      handleBye(payload);      break;
            case SipMsgType::SIP_CANCEL:   handleUpdate(payload);   break; // UPDATE reuses CANCEL slot
            default: break;
        }
    }
}

// ── REGISTER ─────────────────────────────────────────────────
void ScscfNode::handleRegister(const std::vector<uint8_t>& payload) {
    std::string impu, impi, contact;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_IMPU)))    impu    = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_IMPI))) impi  = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CONTACT))) contact = r.readStr();
        else r.skip();
    }

    Logger::scscf("S-CSCF: ← SIP REGISTER  IMPU=" + impu);

    // Cx SAR to HSS
    Logger::scscf("S-CSCF: → Diameter Cx SAR to HSS");
    Logger::ie_field("  Server-Assignment-Type: REGISTRATION");
    Logger::ie_field("  HSS will store: IMPU → this S-CSCF");
    sendCxSAR(impu, impi);
    PcapWriter::instance().writeDiameter(DiameterCmd::SERVER_ASSIGNMENT, DiameterApp::CX, true,
        PcapWriter::IP_SCSCF, PcapWriter::PORT_DIA, PcapWriter::IP_HSS, PcapWriter::PORT_DIA);

    std::string profile;
    if (waitCxSAA(profile)) {
        Logger::scscf("S-CSCF: ← Cx SAA — subscriber profile received");
        Logger::ie_field("  iFC: INVITE→MTAS, REGISTER→MTAS");
        Logger::ie_field("  Profile: " + profile);
        PcapWriter::instance().writeDiameter(DiameterCmd::SERVER_ASSIGNMENT, DiameterApp::CX, false,
            PcapWriter::IP_HSS, PcapWriter::PORT_DIA, PcapWriter::IP_SCSCF, PcapWriter::PORT_DIA);
    }

    // 3rd party REGISTER to MTAS
    Logger::scscf("S-CSCF: → ISC: 3rd-party REGISTER to MTAS");
    Logger::ie_field("  MTAS stores: IMPU=" + impu + " is online at " + contact);
    Logger::ie_field("  MTAS enables: OIP/OIR, call waiting, forwarding for this UE");

    ImsSubscriber sub;
    sub.impu = impu; sub.impi = impi; sub.contact = contact; sub.registered = true;
    registry_[impu] = sub;

    // 200 OK back to P-CSCF → UE
    sendSipResponse(SipMsgType::SIP_200_OK, "reg-"+impu, impu, impu, "REGISTER");
    Logger::scscf("S-CSCF: → SIP 200 OK — " + impu + " registered ✓");
}

// ── INVITE ───────────────────────────────────────────────────
void ScscfNode::handleInvite(const std::vector<uint8_t>& payload) {
    std::string from, to, call_id, sdp;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM)))     from    = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_TO)))  to      = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP))) sdp     = r.readStr();
        else r.skip();
    }

    // Check if re-INVITE (call_id already exists)
    if (calls_.count(call_id)) {
        handleReInvite(payload, call_id, from, sdp);
        return;
    }

    Logger::scscf("S-CSCF: ← SIP INVITE  From=" + from + "  To=" + to);
    Logger::ie_field("  Call-ID: " + call_id);

    // 100 Trying immediately
    sendSipResponse(SipMsgType::SIP_100_TRYING, call_id, from, to, "INVITE");
    Logger::scscf("S-CSCF: → 100 Trying");

    // MTAS invocation via ISC
    bool allowed = invokeMtas(from, to, call_id, sdp);
    if (!allowed) {
        Logger::scscf("S-CSCF: MTAS rejected call (barring) → 603 Decline");
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, from, to, "BARRED");
        return;
    }

    // Store call state
    calls_[call_id] = {from, to, call_id, false, false};

    // Route INVITE to callee (via P-CSCF)
    Logger::scscf("S-CSCF: → routing INVITE to callee " + to);
    Logger::ie_field("  P-CSCF delivers to callee UE socket");
    Logger::ie_field("  UE-B terminal shows incoming call");
    sendToPcscf(payload);

    // 180 Ringing with Require:100rel (reliable provisional response)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sendSipResponse(SipMsgType::SIP_180_RINGING, call_id, from, to, "INVITE");
    Logger::scscf("S-CSCF: → 180 Ringing  (UE-B alerting)");
    Logger::ie_field("  RSeq: 1  (reliable sequence — RFC 3262)");
    Logger::ie_field("  Require: 100rel  (caller MUST send PRACK)");
    Logger::ie_field("  To-tag added — SIP dialog established here");

    // ── PRACK flow (RFC 3262) ─────────────────────────────────
    // Real: UE-A sends PRACK after receiving 180 Ringing
    // PRACK = Provisional Response ACKnowledgement
    // Ensures 180 Ringing is reliably delivered (unlike plain TCP)
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    VLog::step(0, 0, "PRACK  (RFC 3262 — Reliable Provisional Response)",
               "UE-A", Logger::CLR_ENB, "S-CSCF", Logger::CLR_SCSCF)
        .ie("Method",  "PRACK — acknowledges the 180 Ringing")
        .ie("RAck",    "1 1 INVITE  (RSeq=1, CSeq=1, method=INVITE)")
        .ie("Why",     "Plain SIP 1xx are unreliable (UDP). 100rel makes them reliable.")
        .ie("Real",    "Without PRACK: caller doesn't know if callee is actually ringing!")
        .ie("VoLTE",   "3GPP mandates Require:100rel for VoLTE calls (TS 24.229)")
        .next("S-CSCF forwards to callee → callee sends 200 OK to PRACK")
        .flush();
    Logger::scscf("S-CSCF: ← PRACK from caller (SIM: auto-generated)");
    Logger::scscf("S-CSCF: → 200 OK (PRACK) to caller — provisional delivery confirmed");
}

// ── re-INVITE (hold / conference / resume) ────────────────────
void ScscfNode::handleReInvite(const std::vector<uint8_t>& payload,
                                 const std::string& call_id,
                                 const std::string& from,
                                 const std::string& sdp) {
    // Extract To header to know who is being added (conference) or held
    std::string to_impu;
    MessageReader r2(payload);
    while (r2.hasMore()) {
        Tag tag; uint16_t len; if (!r2.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_TO))) { to_impu = r2.readStr(); break; }
        else r2.skip();
    }

    auto& cs = calls_[call_id];

    if (sdp.find("inactive") != std::string::npos || sdp.find("sendonly") != std::string::npos) {
        // ── HOLD ─────────────────────────────────────────────
        Logger::scscf("S-CSCF: ← re-INVITE (HOLD)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: a=sendonly  (caller stops receiving, callee hears hold music)");
        Logger::ie_field("  REAL: a=inactive = full hold, a=sendonly = one-way hold");
        Logger::ie_field("  MTAS: notified via ISC — plays hold music toward callee");
        cs.on_hold = true;
        // Forward re-INVITE to callee so callee UE sees hold state
        sendToPcscf(payload);
        // 200 OK back to caller
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "re-INVITE-HOLD");
        Logger::scscf("S-CSCF: → 200 OK (hold acknowledged)");

    } else if (sdp.find("conf") != std::string::npos) {
        // ── CONFERENCE ───────────────────────────────────────
        Logger::scscf("S-CSCF: ← re-INVITE (CONFERENCE)  Call-ID=" + call_id);
        Logger::ie_field("  To: " + to_impu + "  ← third party being added");
        Logger::ie_field("  MTAS: invoke MRFC via Mr interface (SIP)");
        Logger::ie_field("  MRFC: allocate conference bridge  conf-URI=sip:conf-N@mrfc");
        Logger::ie_field("  MRFC → MRFP: H.248/Megaco — create 3-party audio mixing endpoint");
        cs.in_conference = true;

        // Draw conference diagram
        Diag::ConferenceJoin(cs.caller_impu, cs.callee_impu, to_impu);

        // Send INVITE to third party (UE-C) via P-CSCF
        if (!to_impu.empty()) {
            Logger::scscf("S-CSCF: → INVITE to " + to_impu + " (conference participant)");
            Logger::ie_field("  P-CSCF will deliver to UE-C terminal");
            Logger::ie_field("  UE-C: type ACCEPT to join conference");

            MessageWriter inv(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cs.caller_impu);
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      to_impu);
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), call_id + "-conf");
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
                "audio:50000/AMR-WB/16000;conf;a=sendrecv");
            pcscf_conn_.sendFrame(inv.frame());  // inv.frame() already has length prefix
        }

        // 200 OK to caller (UE-A) — conference initiated
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "CONFERENCE");
        Logger::scscf("S-CSCF: → 200 OK (conference bridge active) to caller");

    } else if (sdp.find("video") != std::string::npos &&
               sdp.find("H264") != std::string::npos) {
        // ── VIDEO SWITCH ─────────────────────────────────────
        bool adding = (sdp.find("port=0") == std::string::npos); // port=0 means removing video
        Logger::scscf("S-CSCF: ← re-INVITE (VIDEO SWITCH)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: m=audio (AMR-WB) + m=video (H264/90000)");
        Logger::ie_field("  MTAS: codec policy check — H264 approved for VoLTE");
        Logger::ie_field("  P-CSCF: Rx AAR update — add video media component");
        Logger::ie_field("  PCRF: install QCI=2 bearer (video) alongside QCI=1 (voice)");
        Diag::VideoSwitch(cs.caller_impu, cs.callee_impu, adding);
        sendToPcscf(payload);
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu,
                        adding ? "VIDEO-ADD" : "VIDEO-REMOVE");

    } else {
        // ── RESUME ───────────────────────────────────────────
        Logger::scscf("S-CSCF: ← re-INVITE (RESUME)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: a=sendrecv  (restoring bidirectional media)");
        cs.on_hold = false;
        sendToPcscf(payload);
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "re-INVITE-RESUME");
        Logger::scscf("S-CSCF: → 200 OK (call resumed)");
    }
}

// ── ACK ──────────────────────────────────────────────────────
void ScscfNode::handleAck(const std::vector<uint8_t>& payload) {
    std::string call_id;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) { call_id = r.readStr(); break; }
        else r.skip();
    }
    Logger::scscf("S-CSCF: ← ACK  Call-ID=" + call_id);
    Logger::ie_field("  SIP 3-way handshake complete — call fully established");
    Logger::ie_field("  MTAS: CDR recording started, QCI=1 bearer confirmed");
    // Forward ACK to callee
    if (calls_.count(call_id)) sendToPcscf(payload);
}

// ── 200 OK from callee ────────────────────────────────────────
void ScscfNode::handle200Ok(const std::vector<uint8_t>& payload) {
    std::string call_id, reason, sdp;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)))  call_id = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_REASON))) reason = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP)))  sdp    = r.readStr();
        else r.skip();
    }
    Logger::scscf("S-CSCF: ← 200 OK from callee  Call-ID=" + call_id);
    if (!sdp.empty()) {
        Logger::ie_field("  SDP answer: " + sdp + "  (codec negotiated)");
        Logger::ie_field("  AMR-WB/16000 = HD Voice selected");
    }
    // Route 200 OK back to caller via P-CSCF
    sendToPcscf(payload);
}

// ── BYE ──────────────────────────────────────────────────────
void ScscfNode::handleBye(const std::vector<uint8_t>& payload) {
    std::string call_id, from;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM))) from = r.readStr();
        else r.skip();
    }

    Logger::scscf("S-CSCF: ← BYE  Call-ID=" + call_id);
    Logger::ie_field("  MTAS: CDR closed, generate billing record");
    Logger::ie_field("  P-CSCF: Rx STR → PCRF → QCI=1 bearer released");

    // Check if this was a conference call leaving
    auto it = calls_.find(call_id);
    if (it != calls_.end() && it->second.in_conference) {
        Logger::scscf("S-CSCF: Conference participant leaving — adjusting MRFC/MRFP");
        // Find the two remaining participants
        std::string remaining1 = it->second.caller_impu;
        std::string remaining2 = it->second.callee_impu;
        Diag::ConferenceLeave(from, remaining1, remaining2);
        Logger::ie_field("  MRFC: notify MRFP to remove one stream (2-party call remains)");
    }

    calls_.erase(call_id);
    sendSipResponse(SipMsgType::SIP_200_OK, call_id, from, "", "BYE");
    sendToPcscf(payload); // forward BYE to peer
}

// ── UPDATE (RFC 3311) ─────────────────────────────────────────
// Mid-dialog session modification. No dialog state change.
// No ACK needed (unlike re-INVITE). Used for:
//   - QoS precondition signalling
//   - Codec renegotiation
//   - Early session modification before ACK
void ScscfNode::handleUpdate(const std::vector<uint8_t>& payload) {
    std::string from, call_id, sdp;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM)))     from    = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r.readStr();
        else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP))) sdp     = r.readStr();
        else r.skip();
    }

    VLog::step(0, 0, "SIP UPDATE  (RFC 3311 — mid-dialog modification)",
               "UE", Logger::CLR_ENB, "S-CSCF → MTAS", Logger::CLR_SCSCF)
        .ie("From",    from)
        .ie("Call-ID", call_id)
        .ie("SDP",     sdp.empty() ? "QoS precondition update" : sdp)
        .ie("Purpose", "Modify session WITHOUT changing dialog state")
        .ie("vs re-INVITE", "UPDATE: no ACK needed, no dialog restart")
        .ie("vs re-INVITE", "re-INVITE: needs ACK, can change dialog")
        .ie("MTAS",    "CDR updated with new codec/QoS parameters")
        .ie("P-CSCF",  "Rx AAR update → PCRF updates bearer policy")
        .next("S-CSCF forwards UPDATE to callee, responds 200 OK with SDP answer")
        .flush();

    Logger::scscf("S-CSCF: → ISC: UPDATE to MTAS (update CDR + codec policy)");
    Logger::mtas("MTAS: ← UPDATE — updating CDR and session parameters");
    Logger::ie_field("  CDR: session modification recorded");
    Logger::ie_field("  Codec policy: re-validated for new SDP");

    // Forward UPDATE to callee
    if (calls_.count(call_id)) sendToPcscf(payload);
    sendSipResponse(SipMsgType::SIP_200_OK, call_id, from, "", "UPDATE");
    Logger::scscf("S-CSCF: → 200 OK (UPDATE) — session modified ✓");
}

// ── MTAS invocation — detailed service logic ──────────────────
// Ericsson MTAS (Multimedia Telephony Application Server)
// Invoked by S-CSCF via ISC interface (SIP) when iFC triggers.
// Each step below is a real MTAS service check.
bool ScscfNode::invokeMtas(const std::string& caller, const std::string& callee,
                             const std::string& call_id, const std::string& sdp) {
    Logger::scscf("S-CSCF: → ISC INVITE to MTAS  [iFC trigger: method=INVITE]");
    Logger::ie_field("  ISC interface: standard SIP between S-CSCF and MTAS");
    Logger::ie_field("  MTAS port: 5080 (or collocated with S-CSCF in Ericsson Cloud IMS)");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── MTAS Step 1: iFC evaluation ───────────────────────────
    Logger::mtas("MTAS: ← ISC INVITE from S-CSCF");
    Logger::ie_field("  iFC match: method=INVITE, role=originating → service invoked");
    Logger::ie_field("  iFC (Initial Filter Criteria) downloaded from HSS via Cx SAA");
    Logger::ie_field("  iFC priority: services checked in priority order");

    // ── MTAS Step 2: OIP (Originating Identity Presentation) ──
    Logger::mtas("MTAS: [1] OIP — Originating Identity Presentation [MMTEL TS 24.173]");
    Logger::ie_field("  Caller IMPU:    " + caller);
    Logger::ie_field("  P-Preferred-ID: caller wants to show their number");
    Logger::ie_field("  OIP result:     ALLOW — caller can present CLI");
    Logger::ie_field("  P-Asserted-ID:  set to caller IMPU (network-verified CLI)");
    Logger::ie_field("  If OIR active:  P-Asserted-ID would be Anonymous");

    // ── MTAS Step 3: Call Barring check ───────────────────────
    Logger::mtas("MTAS: [2] Barring check [MMTEL TS 24.173 §7.8]");
    Logger::ie_field("  OIB (Outgoing International Barring): checking callee number");
    Logger::ie_field("  Callee: " + callee + " → domestic number → NOT barred");
    Logger::ie_field("  BAOC (Bar All Outgoing Calls): NOT active");
    Logger::ie_field("  Result: PASS — call allowed to proceed");

    // ── MTAS Step 4: TIP (Terminating Identity Presentation) ──
    Logger::mtas("MTAS: [3] TIP — Terminating Identity Presentation");
    Logger::ie_field("  Callee: " + callee);
    Logger::ie_field("  TIP check: is callee identity allowed to be shown to caller?");
    Logger::ie_field("  Result: ALLOW — callee number visible to caller");

    // ── MTAS Step 5: Call Forwarding check ────────────────────
    Logger::mtas("MTAS: [4] Call Forwarding [MMTEL TS 24.173 §7.5]");
    Logger::ie_field("  CFU (Unconditional): NOT set for " + callee);
    Logger::ie_field("  CFNRy (No Reply):    NOT set (would activate after 15s)");
    Logger::ie_field("  CFB (Busy):          NOT set (call waiting applies instead)");
    Logger::ie_field("  Result: NO forwarding — route to original callee");

    // ── MTAS Step 6: Call Waiting check ───────────────────────
    Logger::mtas("MTAS: [5] Call Waiting [MMTEL TS 24.173 §7.6]");
    Logger::ie_field("  Checking: is " + callee + " currently in an active call?");
    Logger::ie_field("  Result: NOT in a call — Call Waiting not needed");
    Logger::ie_field("  (If busy: MTAS sends 180 Ringing + call-wait notification)");

    // ── MTAS Step 7: Codec Policy (VoLTE) ─────────────────────
    Logger::mtas("MTAS: [6] MMTEL Codec Policy [TS 26.114]");
    Logger::ie_field("  SDP offer: " + (sdp.empty() ? "audio/AMR-WB + video/H264" : sdp));
    Logger::ie_field("  VoLTE mandate: AMR-WB (wideband) preferred over AMR-NB");
    Logger::ie_field("  Video policy:  H264 baseline allowed");
    Logger::ie_field("  Bandwidth:     AMR-WB=12.65kbps voice, H264≤2Mbps video");
    Logger::ie_field("  Result: SDP offer accepted — AMR-WB/H264 approved");

    // ── MTAS Step 8: CDR creation ─────────────────────────────
    Logger::mtas("MTAS: [7] CDR — Charging Data Record [TS 32.260]");
    Logger::ie_field("  Call-ID:        " + call_id);
    Logger::ie_field("  Caller:         " + caller);
    Logger::ie_field("  Callee:         " + callee);
    Logger::ie_field("  CDR type:       Originating (caller side)");
    Logger::ie_field("  Charging:       IMS Online Charging via Ro interface");
    Logger::ie_field("  Start time:     [timestamp]");
    Logger::ie_field("  CDR state:      OPEN — will be closed on BYE");

    // ── MTAS Step 9: UPDATE handling (QoS preconditions) ──────
    Logger::mtas("MTAS: [8] QoS Preconditions [RFC 3312]");
    Logger::ie_field("  Precondition:  local QoS must be met before alerting callee");
    Logger::ie_field("  Precondition:  remote QoS must be met before alerting callee");
    Logger::ie_field("  In VoLTE:      P-CSCF Rx AAR → PCRF → QCI=1 bearer created");
    Logger::ie_field("  UPDATE method: UE sends UPDATE to signal QoS met (RFC 3311)");
    Logger::ie_field("  SIM:           QoS auto-satisfied — no UPDATE needed");

    // ── MTAS decision ─────────────────────────────────────────
    Logger::mtas("MTAS: → ISC 200 OK to S-CSCF — all checks passed, continue routing");
    Logger::ie_field("  Services applied: OIP, TIP, Barring, CW, Fwd, Codec, CDR");
    Logger::ie_field("  Next: S-CSCF routes INVITE toward callee");

    PcapWriter::instance().writeSIP(
        "INVITE sip:mtas.local SIP/2.0\r\nCall-ID: " + call_id +
        "\r\nX-MTAS-Service: OIP,TIP,Barring,CW,CDR\r\n\r\n",
        PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_MTAS, 5060);

    (void)sdp;
    return true;  // return false to simulate barring rejection
}

void ScscfNode::sendCxSAR(const std::string& impu, const std::string& impi) {
    MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::DIA_CX_SAR)), next_seq_++);
    w.writeStr(static_cast<Tag>(uint16_t(SipTag::CX_IMPU)), impu);
    w.writeStr(static_cast<Tag>(uint16_t(SipTag::CX_IMPI)), impi);
    hss_conn_.sendFrame(w.frame());
}

bool ScscfNode::waitCxSAA(std::string& profile) {
    if (!hss_conn_.hasData(3000)) return false;
    std::vector<uint8_t> payload;
    if (!hss_conn_.recvFrame(payload)) return false;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(uint16_t(SipTag::CX_SCSCF_NAME))) profile = r.readStr();
        else r.skip();
    }
    return true;
}

void ScscfNode::sendSipResponse(SipMsgType type, const std::string& call_id,
                                  const std::string& from, const std::string& to,
                                  const std::string& reason, const std::string& sdp) {
    MessageWriter w(static_cast<MessageType>(uint16_t(type)), next_seq_++);
    w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), call_id);
    w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    from);
    w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      to);
    w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_REASON)),  reason);
    if (!sdp.empty())
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)), sdp);
    pcscf_conn_.sendFrame(w.frame());
}

void ScscfNode::sendToPcscf(const std::vector<uint8_t>& payload) {
    // payload came from recvFrame (NO length prefix) → add it back
    pcscf_conn_.sendPayload(payload);
}
