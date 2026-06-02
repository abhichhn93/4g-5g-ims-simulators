#include "ims/scscf_node.h"
#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/pcap_writer.h"
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
    Logger::ie_field("  P-CSCF will deliver to UE-B socket");
    Logger::ie_field("  UE-B terminal will show incoming call");
    sendToPcscf(payload);

    // Simulate callee ringing (real: P-CSCF delivers, UE-B rings, sends 180)
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    sendSipResponse(SipMsgType::SIP_180_RINGING, call_id, from, to, "INVITE");
    Logger::scscf("S-CSCF: → 180 Ringing  (UE-B alerting)");
    Logger::ie_field("  To-tag added — SIP dialog established here");
}

// ── re-INVITE (hold / conference / resume) ────────────────────
void ScscfNode::handleReInvite(const std::vector<uint8_t>& payload,
                                 const std::string& call_id,
                                 const std::string& from,
                                 const std::string& sdp) {
    auto& cs = calls_[call_id];

    if (sdp.find("inactive") != std::string::npos || sdp.find("sendonly") != std::string::npos) {
        // Hold
        Logger::scscf("S-CSCF: ← re-INVITE (HOLD)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: a=sendonly  (caller still sends, stops receiving)");
        Logger::ie_field("  REAL: a=inactive = full hold, a=sendonly = one-way hold");
        Logger::ie_field("  MTAS: notifies callee UE to play hold music");
        cs.on_hold = true;
        sendToPcscf(payload);  // deliver re-INVITE to callee
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "re-INVITE-HOLD");
        Logger::scscf("S-CSCF: → 200 OK (hold accepted)  callee is on hold");
    } else if (sdp.find("conf") != std::string::npos) {
        // Conference
        Logger::scscf("S-CSCF: ← re-INVITE (CONFERENCE)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: conference URI requested");
        Logger::ie_field("  MTAS: invoke MRFC (Mr interface, SIP) → allocate mixing bridge");
        Logger::ie_field("  MRFC → MRFP: H.248 — create 3-party audio mix endpoint");
        cs.in_conference = true;
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "CONFERENCE");
    } else {
        // Resume
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
void ScscfNode::handleBye(const std::vector<uint8_t>& /*payload*/) {
    Logger::scscf("S-CSCF: ← BYE — call teardown");
    Logger::ie_field("  MTAS: CDR closed, generate billing record");
    Logger::ie_field("  P-CSCF will send Rx STR → PCRF → release QCI=1 bearer");
    sendSipResponse(SipMsgType::SIP_200_OK, "bye", "", "", "BYE");
}

// ── MTAS invocation ───────────────────────────────────────────
bool ScscfNode::invokeMtas(const std::string& caller, const std::string& callee,
                             const std::string& call_id, const std::string& sdp) {
    Logger::scscf("S-CSCF: → ISC: INVITE to MTAS (Ericsson Application Server)");
    Logger::ie_field("  iFC trigger: method=INVITE → invoke MTAS");

    VLog::step(0, 0, "MTAS SERVICE LOGIC  [ISC interface]",
               "S-CSCF", Logger::CLR_SCSCF, "MTAS", Logger::CLR_MTAS)
        .ie("OIP",     "Caller " + caller + " — CLI presentation allowed")
        .ie("Barring", "OIB check: callee " + callee + " — not international — PASS")
        .ie("CW",      "Callee not in active call — no call waiting needed")
        .ie("Fwd",     "No call forwarding active for " + callee)
        .ie("CDR",     "Charging Data Record started  Call-ID=" + call_id)
        .ie("MMTEL",   "MMTEL service features applied — codec policy: AMR-WB preferred")
        .next("MTAS returns 'continue' to S-CSCF — routing proceeds")
        .flush();

    PcapWriter::instance().writeSIP(
        "INVITE sip:mtas.local SIP/2.0\r\nCall-ID: " + call_id + "\r\n\r\n",
        PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_MTAS, 5060);
    (void)sdp;
    return true; // return false to simulate barring
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

void ScscfNode::sendToPcscf(const std::vector<uint8_t>& frame) {
    pcscf_conn_.sendFrame(frame);
}
