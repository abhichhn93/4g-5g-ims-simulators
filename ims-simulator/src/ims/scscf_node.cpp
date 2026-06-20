#include "ims/scscf_node.h"
#include "ims/mtas_state.h"
#include "ims/sip_text.h"
#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/exceptions.h"
#include "common/pcap_writer.h"
#include "ims/ims_diagrams.h"
#include <chrono>
#include <thread>
#include <future>
#include <shared_mutex>
#include <stdexcept>

static constexpr const char* SCSCF_IP   = "127.0.0.1";
static constexpr uint16_t    SCSCF_PORT  = 5070;
static constexpr const char* HSS_CX_IP  = "127.0.0.1";
static constexpr uint16_t    HSS_CX_PORT = 3870;

ScscfNode::ScscfNode(std::atomic<bool>& stop, std::atomic<bool>& scscf_ready,
                     std::atomic<bool>& hss_ready)
    : stop_(stop), scscf_ready_(scscf_ready), hss_ready_(hss_ready) {}

void ScscfNode::run() {
    Logger::scscf(Logger::Level::SYSTEM, "S-CSCF: starting — SIP registrar + MTAS gateway");
    try { 
        setupServer(); 
        if (!stop_.load()) {
            scscf_ready_.store(true); 
            receiveLoop(); 
        }
    }
    catch (const std::exception& e) { 
        Logger::scscf(Logger::Level::INTERVIEW_C, 
            "Exception Caught: Thread-level 'try-catch' is critical in Telecom nodes "
            "to prevent a single bad packet or socket error from crashing the process.");
        Logger::warn("S-CSCF", e.what()); 
    }
    Logger::scscf(Logger::Level::SYSTEM, "S-CSCF: thread exiting");
}

void ScscfNode::setupServer() {
    server_socket_ = Socket::createServer(SCSCF_IP, SCSCF_PORT);
    
    // SENIOR TIP: Using atomic load in the retry loop
    for (int i = 0; i < 50 && !stop_.load(); ++i) {
        try { hss_conn_ = Socket::connectTo(HSS_CX_IP, HSS_CX_PORT); break; }
        catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    
    Logger::scscf(Logger::Level::SYSTEM, "S-CSCF: IMS-HSS Cx link UP ✓");
    
    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            pcscf_conn_ = server_socket_.accept();
            Logger::scscf(Logger::Level::SYSTEM, "S-CSCF: P-CSCF connected ✓");
            return;
        }
    }
}

void ScscfNode::receiveLoop() {
    Logger::scscf(Logger::Level::SYSTEM, "S-CSCF: receive loop started");
    while (!stop_.load()) {
        if (!pcscf_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!pcscf_conn_.recvFrame(payload)) break;

        try {
            MessageReader r(payload);
            auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));
            switch (type) {
                case SipMsgType::SIP_REGISTER: handleRegister(payload); break;
                case SipMsgType::SIP_INVITE:   handleInvite(payload);   break;
                case SipMsgType::SIP_ACK:      handleAck(payload);      break;
                case SipMsgType::SIP_200_OK:   handle200Ok(payload);    break;
                case SipMsgType::SIP_BYE:      handleBye(payload);      break;
                case SipMsgType::SIP_CANCEL:   handleUpdate(payload);   break;
                default: 
                    throw ProtocolException("Unrecognized SIP Message Type received at S-CSCF");
            }
        } catch (const ProtocolException& e) {
            Logger::warn("S-CSCF", std::string("Protocol Violation: ") + e.what());
            // In a real node, we would send a 400 Bad Request here
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

    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← SIP REGISTER  IMPU=" + impu);

    // Cx SAR to HSS
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → Diameter Cx SAR to HSS");
    Logger::ie_field("  Server-Assignment-Type: REGISTRATION");
    Logger::ie_field("  HSS will store: IMPU → this S-CSCF");
    sendCxSAR(impu, impi);
    PcapWriter::instance().writeDiameter(DiameterCmd::SERVER_ASSIGNMENT, DiameterApp::CX, true,
        PcapWriter::IP_SCSCF, PcapWriter::PORT_DIA, PcapWriter::IP_HSS, PcapWriter::PORT_DIA);

    std::string profile;
    if (waitCxSAA(profile)) {
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← Cx SAA — subscriber profile received");
        Logger::ie_field("  iFC: INVITE→MTAS, REGISTER→MTAS");
        Logger::ie_field("  Profile: " + profile);
        PcapWriter::instance().writeDiameter(DiameterCmd::SERVER_ASSIGNMENT, DiameterApp::CX, false,
            PcapWriter::IP_HSS, PcapWriter::PORT_DIA, PcapWriter::IP_SCSCF, PcapWriter::PORT_DIA);
    }

    // 3rd party REGISTER to MTAS
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → ISC: 3rd-party REGISTER to MTAS");
    Logger::ie_field("  MTAS stores: IMPU=" + impu + " is online at " + contact);
    Logger::ie_field("  MTAS enables: OIP/OIR, call waiting, forwarding for this UE");

    ImsSubscriber sub;
    sub.impu = impu; sub.impi = impi; sub.contact = contact; sub.registered = true;
    
    {
        // INTERVIEW: std::unique_lock vs std::shared_lock
        // We use unique_lock because we are WRITING to the registry.
        // Use {} instead of () to avoid "Most Vexing Parse" errors.
        std::unique_lock<std::shared_mutex> lock{registry_mtx_};
        registry_[impu] = sub;
    }

    // 200 OK back to P-CSCF → UE
    sendSipResponse(SipMsgType::SIP_200_OK, "reg-"+impu, impu, impu, "REGISTER");
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → SIP 200 OK — " + impu + " registered ✓");
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
    {
        std::shared_lock<std::shared_mutex> lock(calls_mtx_);
        if (calls_.count(call_id)) {
            handleReInvite(payload, call_id, from, sdp);
            return;
        }
    }
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← SIP INVITE  From=" + from + "  To=" + to);
    Logger::ie_field("  Call-ID: " + call_id);

    // 100 Trying immediately
    sendSipResponse(SipMsgType::SIP_100_TRYING, call_id, from, to, "INVITE");
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 100 Trying");
    PcapWriter::instance().writeSIP(
        SipText::build100Trying(from, to, call_id, 1),
        PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);

    // INTERVIEW: std::async (Non-blocking Service Trigger)
    // In high-scale nodes, we don't block the signaling thread for MTAS lookups.
    auto mtas_check = std::async(std::launch::async, [this, from, to, call_id, sdp]() {
        return invokeMtas(from, to, call_id, sdp);
    });

    if (!mtas_check.get()) {
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: MTAS rejected call (barring) → 603 Decline");
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, from, to, "BARRED");
        return;
    }

    // Store call state
    {
        std::unique_lock<std::shared_mutex> lock(calls_mtx_);
        calls_[call_id] = {from, to, call_id, false, false};
    }

    // Route INVITE to callee (via P-CSCF)
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → routing INVITE to callee " + to);
    Logger::ie_field("  P-CSCF delivers to callee UE socket");
    Logger::ie_field("  UE-B terminal shows incoming call");
    sendToPcscf(payload);
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: added hop headers before routing to callee →");
    Logger::ie_field("  + Record-Route: <sip:scscf.ims...;lr>  ← stays in dialog path for BYE/re-INVITE");
    Logger::ie_field("  + P-Asserted-Identity: " + from + "  ← MTAS OIP verified CLI");
    Logger::ie_field("  + P-Charging-Vector: icid-value=charge-" + call_id);
    Logger::ie_field("  + Via: SIP/2.0/TCP 10.0.0.9:5070;branch=z9hG4bK-scscf");
    Logger::ie_field("  Why Record-Route: S-CSCF must stay in path for BYE/re-INVITE policy enforcement");
    Logger::scscf(Logger::Level::BEGINNER, "S-CSCF verified your caller ID and added its routing tag — it watches the whole call");

    // NOTE: 180 Ringing is now sent by P-CSCF itself, the moment the INVITE
    // is actually delivered to the callee's UE socket (see
    // PcscfNode::handleFromScscf's SIP_INVITE case) — not on a fixed timer
    // here. This makes "ringing" causally tied to real delivery.

    // ── 183 Session Progress (RFC 3312 — QoS Preconditions) ─────
    // Sent BEFORE 180 Ringing. Carries SDP answer with QoS attributes.
    // Network must reserve QCI=1 bearer BEFORE alerting callee.
    // RSeq header triggers PRACK (100rel). Real VoLTE flow:
    //   INVITE → 100 Trying → 183 Session Progress → PRACK →
    //   200 OK PRACK → (bearer created) → 180 Ringing → 200 OK → ACK
    sendSipResponse(SipMsgType::SIP_183_PROGRESS, call_id, from, to, "INVITE");
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 183 Session Progress  [RFC 3312 QoS preconditions]");
    Logger::ie_field("  RSeq: 1  (Reliability Sequence — triggers PRACK from caller)");
    Logger::ie_field("  Require: 100rel  (3GPP mandatory for VoLTE, TS 24.229)");
    Logger::ie_field("  SDP: early media answer + a=curr:qos (not yet reserved)");
    Logger::ie_field("  a=des:qos mandatory — bearer MUST be created before 180 Ringing");
    Logger::ie_field("  P-CSCF reads this SDP → sends Rx AAR to PCRF → QCI=1 bearer");
    Logger::scscf(Logger::Level::BEGINNER,
        "Network reserving voice-quality channel (QCI=1) before ringing callee");
    PcapWriter::instance().writeSIP(
        SipText::build183SessionProgress(from, to, call_id, 1),
        PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);

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
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← PRACK from caller (SIM: auto-generated)");
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 200 OK (PRACK) to caller — provisional delivery confirmed");
    PcapWriter::instance().writeSIP(
        SipText::buildPrack(from, to, call_id, 1),
        PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_SCSCF, 5060);
    PcapWriter::instance().writeSIP(
        SipText::build200Prack(to, from, call_id, 1),
        PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
}

// ── re-INVITE (hold / conference / resume) ────────────────────
void ScscfNode::handleReInvite(const std::vector<uint8_t>& payload,
                                 const std::string& call_id,
                                 const std::string& from,
                                 const std::string& sdp) {
    // INTERVIEW: No manual locking here because handleInvite already holds the lock or
    // we are in a read-only section of a specific dialog context.

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
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← re-INVITE (HOLD)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: a=sendonly  (caller stops receiving, callee hears hold music)");
        Logger::ie_field("  REAL: a=inactive = full hold, a=sendonly = one-way hold");
        Logger::ie_field("  MTAS: notified via ISC — plays hold music toward callee");
        cs.on_hold = true;
        Logger::scscf(Logger::Level::BEGINNER, "Call put on hold — the other side will hear hold music");
        VLog::step(0, 0, "HOLD  (SDP mid-dialog modification)",
                   cs.caller_impu, Logger::CLR_ENB, cs.callee_impu, Logger::CLR_ENB)
            .ie("Before SDP", "a=sendrecv  (bidirectional)")
            .ie("After SDP",  "a=sendonly  (caller sends only, callee cannot send)")
            .ie("Hold music", "MTAS/MRFP plays comfort tone toward callee")
            .ie("Bearer",     "P-CSCF: Rx AAR update → PCRF reduces QCI=1 to one-way")
            .next("Type RESUME to restore bidirectional voice")
            .flush();
        // Forward re-INVITE to callee so callee UE sees hold state
        sendToPcscf(payload);
        // 200 OK back to caller
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "re-INVITE-HOLD");
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 200 OK (hold acknowledged)");

    } else if (sdp.find("conf") != std::string::npos) {
        // ── CONFERENCE ───────────────────────────────────────
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← re-INVITE (CONFERENCE)  Call-ID=" + call_id);
        Logger::ie_field("  To: " + to_impu + "  ← third party being added");
        Logger::ie_field("  MTAS: invoke MRFC via Mr interface (SIP)");
        Logger::ie_field("  MRFC: allocate conference bridge  conf-URI=sip:conf-N@mrfc");
        Logger::ie_field("  MRFC → MRFP: H.248/Megaco — create 3-party audio mixing endpoint");
        cs.in_conference = true;

        VLog::step(0, 0, "3-PARTY CONFERENCE SETUP via MRFC",
                   cs.caller_impu, Logger::CLR_ENB, "MRFC", Logger::CLR_MTAS)
            .ie("Method",       "re-INVITE (SIM) — real IMS uses REFER/NOTIFY (RFC 3515)")
            .ie("MRFC",         "conference bridge at sip:conf-N@mrfc.ims — allocates mix endpoint")
            .ie("H.248/Megaco", "MRFC → MRFP: create 3-party audio mixing stream")
            .ie("REFER/NOTIFY", "Real: UE-A sends REFER Refer-To:UE-C; S-CSCF sends INVITE to UE-C")
            .ie("NOTIFY",       "S-CSCF → UE-A: subscription state (trying/early/terminated)")
            .ie("SUBSCRIBE",    "UE-A subscribes to conference-state (RFC 4575) at MRFC URI")
            .ie("NOTIFY+XML",   "MRFC → UE-A: conference-info+xml listing all participants")
            .next("S-CSCF sending INVITE to " + to_impu + " — wait for ACCEPT")
            .flush();
        Logger::scscf(Logger::Level::BEGINNER, "Setting up 3-way call — inviting a third person to join");

        // Draw conference diagram
        Diag::ConferenceJoin(cs.caller_impu, cs.callee_impu, to_impu);

        // ── PCAP: REFER → 202 Accepted → NOTIFY chain ───────────
        // Real IMS: UE-A sends REFER to S-CSCF with Refer-To: UE-C
        static const std::string CONF_URI = "sip:conf-1@mrfc.ims.mnc010.mcc404.3gppnetwork.org";
        PcapWriter::instance().writeSIP(
            SipText::buildRefer(cs.caller_impu, cs.callee_impu, to_impu, call_id, 2),
            PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_SCSCF, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← REFER (add " + to_impu + " to call)");
        Logger::ie_field("  Refer-To: " + to_impu + "  ← who to add");
        Logger::ie_field("  Referred-By: " + cs.caller_impu);

        // 202 Accepted (async — NOTIFY will follow)
        PcapWriter::instance().writeSIP(
            SipText::build202Accepted(cs.callee_impu, cs.caller_impu, call_id, 2),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 202 Accepted (REFER accepted, sending INVITE to UE-C)");
        Logger::ie_field("  202 not 200 — REFER is async, NOTIFY will report result");

        // NOTIFY: trying (S-CSCF → UE-A — INVITE sent to UE-C)
        PcapWriter::instance().writeSIP(
            SipText::buildNotifyRefer(cs.caller_impu, cs.callee_impu, call_id, 1,
                "active", "SIP/2.0 100 Trying"),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → NOTIFY (REFER state: trying — INVITE sent to UE-C)");

        // ── PCAP: SUBSCRIBE → 200 OK → NOTIFY (conference-state) ─
        PcapWriter::instance().writeSIP(
            SipText::buildSubscribe(cs.caller_impu, CONF_URI, call_id, 3),
            PcapWriter::IP_UE, 5060, PcapWriter::IP_MRFC, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "MRFC: ← SUBSCRIBE (conference-state)  Event: conference");
        Logger::ie_field("  UE-A subscribes to get participant list updates");
        Logger::ie_field("  Accept: application/conference-info+xml");

        PcapWriter::instance().writeSIP(
            SipText::build200Register(cs.caller_impu, "10.0.0.11", 3),
            PcapWriter::IP_MRFC, 5060, PcapWriter::IP_UE, 5060);

        // ── PCAP: INVITE to UE-C (conference leg) ────────────────
        PcapWriter::instance().writeSIP(
            SipText::buildInvite(cs.caller_impu, to_impu, "10.0.0.11", call_id + "-conf", 1),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → INVITE to UE-C  Call-ID=" + call_id + "-conf");

        // 100 Trying for conference INVITE
        PcapWriter::instance().writeSIP(
            SipText::build100Trying(cs.caller_impu, to_impu, call_id + "-conf", 1),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);

        // 183 Session Progress for conference leg
        PcapWriter::instance().writeSIP(
            SipText::build183SessionProgress(cs.caller_impu, to_impu, call_id + "-conf", 1, 70000),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);

        // PRACK for conference
        PcapWriter::instance().writeSIP(
            SipText::buildPrack(cs.caller_impu, to_impu, call_id + "-conf", 1),
            PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_SCSCF, 5060);
        PcapWriter::instance().writeSIP(
            SipText::build200Prack(to_impu, cs.caller_impu, call_id + "-conf", 1),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);

        // NOTIFY: early (UE-C is ringing)
        PcapWriter::instance().writeSIP(
            SipText::buildNotifyRefer(cs.caller_impu, cs.callee_impu, call_id, 2,
                "active", "SIP/2.0 180 Ringing"),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → NOTIFY (REFER state: early — UE-C ringing)");

        // 180 Ringing for UE-C
        PcapWriter::instance().writeSIP(
            SipText::build180Ringing(cs.caller_impu, to_impu, call_id + "-conf", 1),
            PcapWriter::IP_PCSCF, 5060, PcapWriter::IP_UE_C, 5060);

        // Send INVITE to third party (UE-C) via P-CSCF (actual binary message)
        if (!to_impu.empty()) {
            Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → INVITE to " + to_impu + " (conference participant)");
            Logger::ie_field("  P-CSCF will deliver to UE-C terminal");
            Logger::ie_field("  UE-C: type ACCEPT to join conference");

            MessageWriter inv(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cs.caller_impu);
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      to_impu);
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), call_id + "-conf");
            inv.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
                "audio:50000/AMR-WB/16000;conf;a=sendrecv");
            pcscf_conn_.sendFrame(inv.frame());
        }

        // 200 OK to caller (UE-A) — conference initiated
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "CONFERENCE");
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 200 OK (conference bridge active) to caller");

        // ── PCAP: 200 OK + ACK for conference leg ────────────────
        PcapWriter::instance().writeSIP(
            SipText::build200Invite(cs.caller_impu, to_impu, "10.0.0.3", call_id + "-conf", 1, 70000),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
        PcapWriter::instance().writeSIP(
            SipText::buildAck(cs.caller_impu, to_impu, call_id + "-conf", 1),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_UE_C, 5060);

        // NOTIFY: terminated (REFER fully completed, UE-C joined)
        PcapWriter::instance().writeSIP(
            SipText::buildNotifyRefer(cs.caller_impu, cs.callee_impu, call_id, 3,
                "terminated;reason=noresource", "SIP/2.0 200 OK"),
            PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → NOTIFY (REFER terminated — UE-C joined, conference active)");
        Logger::ie_field("  Body: SIP/2.0 200 OK (UE-C answered INVITE)");
        Logger::ie_field("  Subscription-State: terminated;reason=noresource");

        // NOTIFY: conference-state XML (all 3 participants listed)
        PcapWriter::instance().writeSIP(
            SipText::buildNotifyConf(CONF_URI, cs.caller_impu, call_id, 2,
                cs.caller_impu, cs.callee_impu, to_impu),
            PcapWriter::IP_MRFC, 5060, PcapWriter::IP_PCSCF, 5060);
        Logger::scscf(Logger::Level::ENGINEER, "MRFC: → NOTIFY (conference-state XML — 3 participants)");
        Logger::ie_field("  Content-Type: application/conference-info+xml");
        Logger::ie_field("  Body: lists UE-A, UE-B, UE-C as 'connected'");
        Logger::scscf(Logger::Level::BEGINNER, "3-way conference active — all 3 can hear each other via MRFP");

    } else if (sdp.find("video") != std::string::npos &&
               sdp.find("H264") != std::string::npos) {
        // ── VIDEO SWITCH ─────────────────────────────────────
        bool adding = (sdp.find("port=0") == std::string::npos); // port=0 means removing video
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← re-INVITE (VIDEO SWITCH)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: m=audio (AMR-WB) + m=video (H264/90000)");
        Logger::ie_field("  MTAS: codec policy check — H264 approved for VoLTE");
        Logger::ie_field("  P-CSCF: Rx AAR update — add video media component");
        Logger::ie_field("  PCRF: install QCI=2 bearer (video) alongside QCI=1 (voice)");
        if (adding) {
            Logger::scscf(Logger::Level::BEGINNER, "Video added to the voice call");
            VLog::step(0, 0, "VIDEO UPGRADE  (re-INVITE)",
                       cs.caller_impu, Logger::CLR_ENB, cs.callee_impu, Logger::CLR_ENB)
                .ie("Before SDP", "m=audio only  (AMR-WB)")
                .ie("After SDP",  "m=audio AMR-WB  + m=video H264/90000")
                .ie("Bearer",     "P-CSCF: Rx AAR update → PCRF adds QCI=2 video bearer")
                .flush();
        } else {
            Logger::scscf(Logger::Level::BEGINNER, "Video dropped — voice-only call");
            VLog::step(0, 0, "VIDEO REMOVE  (re-INVITE)",
                       cs.caller_impu, Logger::CLR_ENB, cs.callee_impu, Logger::CLR_ENB)
                .ie("Before SDP", "m=audio + m=video H264")
                .ie("After SDP",  "m=audio  + m=video port=0  (port=0 = remove media)")
                .ie("Bearer",     "P-CSCF: Rx STR for video → PCRF releases QCI=2 bearer")
                .flush();
        }
        Diag::VideoSwitch(cs.caller_impu, cs.callee_impu, adding);
        sendToPcscf(payload);
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu,
                        adding ? "VIDEO-ADD" : "VIDEO-REMOVE");

    } else {
        // ── RESUME ───────────────────────────────────────────
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← re-INVITE (RESUME)  Call-ID=" + call_id);
        Logger::ie_field("  SDP: a=sendrecv  (restoring bidirectional media)");
        cs.on_hold = false;
        Logger::scscf(Logger::Level::BEGINNER, "Call resumed — voice is bidirectional again");
        VLog::step(0, 0, "RESUME  (SDP restore)",
                   cs.caller_impu, Logger::CLR_ENB, cs.callee_impu, Logger::CLR_ENB)
            .ie("Before SDP", "a=sendonly  (hold)")
            .ie("After SDP",  "a=sendrecv  (bidirectional restored)")
            .ie("Bearer",     "P-CSCF: Rx AAR update → PCRF restores QCI=1 to bidirectional")
            .flush();
        sendToPcscf(payload);
        sendSipResponse(SipMsgType::SIP_200_OK, call_id, cs.caller_impu, cs.callee_impu, "re-INVITE-RESUME");
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 200 OK (call resumed)");
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
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← ACK  Call-ID=" + call_id);
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
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← 200 OK from callee  Call-ID=" + call_id);
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

    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: ← BYE  Call-ID=" + call_id);
    Logger::ie_field("  MTAS: CDR closed, generate billing record");
    Logger::ie_field("  P-CSCF: Rx STR → PCRF → QCI=1 bearer released");

    // Check if this was a conference call leaving
    auto it = calls_.find(call_id);
    if (it != calls_.end() && it->second.in_conference) {
        Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: Conference participant leaving — adjusting MRFC/MRFP");
        // Find the two remaining participants
        std::string remaining1 = it->second.caller_impu;
        std::string remaining2 = it->second.callee_impu;
        Diag::ConferenceLeave(from, remaining1, remaining2);
        Logger::ie_field("  MRFC: notify MRFP to remove one stream (2-party call remains)");
    }

    calls_.erase(call_id);
    sendSipResponse(SipMsgType::SIP_200_OK, call_id, from, "", "BYE");
    PcapWriter::instance().writeSIP(
        SipText::build200Bye(from, "", call_id, 1),
        PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
    sendToPcscf(payload); // forward BYE to peer
    PcapWriter::instance().writeSIP(
        SipText::buildBye(from, "", call_id, 1),
        PcapWriter::IP_SCSCF, 5060, PcapWriter::IP_PCSCF, 5060);
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

    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → ISC: UPDATE to MTAS (update CDR + codec policy)");
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: ← UPDATE — updating CDR and session parameters");
    Logger::ie_field("  CDR: session modification recorded");
    Logger::ie_field("  Codec policy: re-validated for new SDP");

    // Forward UPDATE to callee
    if (calls_.count(call_id)) sendToPcscf(payload);
    sendSipResponse(SipMsgType::SIP_200_OK, call_id, from, "", "UPDATE");
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → 200 OK (UPDATE) — session modified ✓");
}

// ── MTAS invocation — detailed service logic ──────────────────
// Ericsson MTAS (Multimedia Telephony Application Server)
// Invoked by S-CSCF via ISC interface (SIP) when iFC triggers.
// Each step below is a real MTAS service check.
[[nodiscard]] 
bool ScscfNode::invokeMtas(const std::string& caller, const std::string& callee,
                             const std::string& call_id, const std::string& sdp) {
    Logger::scscf(Logger::Level::ENGINEER, "S-CSCF: → ISC INVITE to MTAS  [iFC trigger: method=INVITE]");
    Logger::ie_field("  ISC interface: standard SIP between S-CSCF and MTAS");
    Logger::ie_field("  MTAS port: 5080 (or collocated with S-CSCF in Ericsson Cloud IMS)");

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ── MTAS Step 1: iFC evaluation ───────────────────────────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: ← ISC INVITE from S-CSCF");
    Logger::ie_field("  iFC match: method=INVITE, role=originating → service invoked");
    Logger::ie_field("  iFC (Initial Filter Criteria) downloaded from HSS via Cx SAA");
    Logger::ie_field("  iFC priority: services checked in priority order");

    // ── MTAS Step 2: OIP (Originating Identity Presentation) ──
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [1] OIP — Originating Identity Presentation [MMTEL TS 24.173]");
    Logger::ie_field("  Caller IMPU:    " + caller);
    Logger::ie_field("  P-Preferred-ID: caller wants to show their number");
    Logger::ie_field("  OIP result:     ALLOW — caller can present CLI");
    Logger::ie_field("  P-Asserted-ID:  set to caller IMPU (network-verified CLI)");
    Logger::ie_field("  If OIR active:  P-Asserted-ID would be Anonymous");

    // ── MTAS Step 3: Call Barring check ───────────────────────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [2] Barring check [MMTEL TS 24.173 §7.8]");
    Logger::ie_field("  OIB (Outgoing International Barring): checking callee number");
    Logger::ie_field("  Callee: " + callee + " → domestic number → NOT barred");
    Logger::ie_field("  BAOC (Bar All Outgoing Calls): NOT active");
    Logger::ie_field("  Result: PASS — call allowed to proceed");

    if (MtasState::isBarred(callee)) {
        Logger::mtas(Logger::Level::ENGINEER,
            "MTAS: [2b] BAOC active for " + callee + " — returning 603 Decline");
        Logger::ie_field("  BAOC: Bar All Outgoing Calls (or BAIC on incoming side)");
        Logger::ie_field("  Result: REJECT — S-CSCF will send 603 Decline to caller");
        Logger::mtas(Logger::Level::BEGINNER,
            "This number has call barring turned on — the call cannot go through");
        return false;
    }

    // ── MTAS Step 4: TIP (Terminating Identity Presentation) ──
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [3] TIP — Terminating Identity Presentation");
    Logger::ie_field("  Callee: " + callee);
    Logger::ie_field("  TIP check: is callee identity allowed to be shown to caller?");
    Logger::ie_field("  Result: ALLOW — callee number visible to caller");

    // ── MTAS Step 5: Call Forwarding check ────────────────────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [4] Call Forwarding [MMTEL TS 24.173 §7.5]");
    Logger::ie_field("  CFU (Unconditional): NOT set for " + callee);
    Logger::ie_field("  CFNRy (No Reply):    NOT set (would activate after 15s)");
    Logger::ie_field("  CFB (Busy):          NOT set (call waiting applies instead)");
    Logger::ie_field("  Result: NO forwarding — route to original callee");

    // ── MTAS Step 6: Call Waiting check ───────────────────────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [5] Call Waiting [MMTEL TS 24.173 §7.6]");
    Logger::ie_field("  Checking: is " + callee + " currently in an active call?");
    Logger::ie_field("  Result: NOT in a call — Call Waiting not needed");
    Logger::ie_field("  (If busy: MTAS sends 180 Ringing + call-wait notification)");

    // ── MTAS Step 7: Codec Policy (VoLTE) ─────────────────────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [6] MMTEL Codec Policy [TS 26.114]");
    Logger::ie_field("  SDP offer: " + (sdp.empty() ? "audio/AMR-WB + video/H264" : sdp));
    Logger::ie_field("  VoLTE mandate: AMR-WB (wideband) preferred over AMR-NB");
    Logger::ie_field("  Video policy:  H264 baseline allowed");
    Logger::ie_field("  Bandwidth:     AMR-WB=12.65kbps voice, H264≤2Mbps video");
    Logger::ie_field("  Result: SDP offer accepted — AMR-WB/H264 approved");

    // ── MTAS Step 8: CDR creation ─────────────────────────────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [7] CDR — Charging Data Record [TS 32.260]");
    Logger::ie_field("  Call-ID:        " + call_id);
    Logger::ie_field("  Caller:         " + caller);
    Logger::ie_field("  Callee:         " + callee);
    Logger::ie_field("  CDR type:       Originating (caller side)");
    Logger::ie_field("  Charging:       IMS Online Charging via Ro interface");
    Logger::ie_field("  Start time:     [timestamp]");
    Logger::ie_field("  CDR state:      OPEN — will be closed on BYE");

    // ── MTAS Step 9: UPDATE handling (QoS preconditions) ──────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: [8] QoS Preconditions [RFC 3312]");
    Logger::ie_field("  Precondition:  local QoS must be met before alerting callee");
    Logger::ie_field("  Precondition:  remote QoS must be met before alerting callee");
    Logger::ie_field("  In VoLTE:      P-CSCF Rx AAR → PCRF → QCI=1 bearer created");
    Logger::ie_field("  UPDATE method: UE sends UPDATE to signal QoS met (RFC 3311)");
    Logger::ie_field("  SIM:           QoS auto-satisfied — no UPDATE needed");

    // ── MTAS decision ─────────────────────────────────────────
    Logger::mtas(Logger::Level::ENGINEER, "MTAS: → ISC 200 OK to S-CSCF — all checks passed, continue routing");
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
