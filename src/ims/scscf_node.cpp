#include "ims/scscf_node.h"
#include "common/logger.h"
#include <chrono>
#include <thread>
#include <stdexcept>

static constexpr const char* SCSCF_IP  = "127.0.0.1";
static constexpr uint16_t    SCSCF_PORT = 5070;
static constexpr const char* HSS_CX_IP  = "127.0.0.1";
static constexpr uint16_t    HSS_CX_PORT = 3870;  // IMS HSS Cx port

ScscfNode::ScscfNode(std::atomic<bool>& stop, std::atomic<bool>& scscf_ready,
                     std::atomic<bool>& hss_ready)
    : stop_(stop), scscf_ready_(scscf_ready), hss_ready_(hss_ready)
{}

void ScscfNode::run() {
    Logger::scscf("S-CSCF: thread started");
    Logger::scscf("S-CSCF: REAL: Ericsson MTAS is an AS invoked by S-CSCF via ISC interface.");
    Logger::scscf("S-CSCF: OUR SIM: S-CSCF + MTAS combined — invokeMtas() shows what MTAS does.");
    try {
        setupServer();
        if (!stop_.load()) receiveLoop();
    } catch (const std::exception& e) {
        Logger::warn("S-CSCF", e.what());
    }
    Logger::scscf("S-CSCF: thread exiting");
}

void ScscfNode::setupServer() {
    Logger::scscf("S-CSCF: binding TCP on port " + std::to_string(SCSCF_PORT));
    server_socket_ = Socket::createServer(SCSCF_IP, SCSCF_PORT);

    Logger::scscf("S-CSCF: connecting to IMS-HSS (Cx interface) on port " + std::to_string(HSS_CX_PORT));
    for (int i = 0; i < 50 && !stop_.load(); ++i) {
        try { hss_conn_ = Socket::connectTo(HSS_CX_IP, HSS_CX_PORT); break; }
        catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(100)); }
    }
    Logger::scscf("S-CSCF: IMS-HSS Cx link UP ✓");
    scscf_ready_.store(true);

    Logger::scscf("S-CSCF: waiting for P-CSCF to connect...");
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
            case SipMsgType::SIP_BYE:      handleBye(payload);      break;
            default: break;
        }
    }
}

void ScscfNode::handleRegister(const std::vector<uint8_t>& payload) {
    std::string impu, impi, contact;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_IMPU)))
            impu = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_IMPI)))
            impi = r.readStr();
        else if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CONTACT)))
            contact = r.readStr();
        else r.skip();
    }

    Logger::scscf("S-CSCF: ← SIP REGISTER from P-CSCF");
    Logger::ie_field("  IMPU: " + impu);
    Logger::ie_field("  IMPI: " + impi);
    Logger::ie_field("  Contact: " + contact);

    // ── Step 1: Cx SAR to HSS ────────────────────────────────
    Logger::scscf("S-CSCF: → Diameter Cx SAR to HSS [TS 29.229 §6.1.3]");
    Logger::ie_field("  Server-Assignment-Type: REGISTRATION");
    Logger::ie_field("  S-CSCF registers itself as serving node for this IMPU");
    Logger::ie_field("  HSS stores: IMPU → S-CSCF name mapping");
    sendCxSAR(impu, impi);

    std::string profile;
    if (waitCxSAA(profile)) {
        Logger::scscf("S-CSCF: ← Diameter Cx SAA from HSS");
        Logger::ie_field("  Subscriber profile received: " + profile);
        Logger::ie_field("  Service Profile contains: Initial Filter Criteria (iFC)");
        Logger::ie_field("  iFC defines WHEN to invoke MTAS (trigger points)");
        Logger::scscf("S-CSCF: REAL: iFC example trigger: 'If REGISTER method → invoke MTAS AS'");
    }

    // ── Step 2: Invoke MTAS (Third Party Registration) ───────
    Logger::scscf("S-CSCF: → ISC: Third Party REGISTER to MTAS (Ericsson AS)");
    Logger::scscf("S-CSCF: REAL: S-CSCF sends a copy of REGISTER to MTAS via ISC interface");
    Logger::scscf("S-CSCF: MTAS stores: IMPU, MSISDN, service profile, registered contact");
    Logger::scscf("S-CSCF: MTAS enables: OIP/OIR, CLIP, call waiting, forwarding for this user");

    // ── Step 3: Store registration ────────────────────────────
    ImsSubscriber sub;
    sub.impu = impu; sub.impi = impi; sub.contact = contact; sub.registered = true;
    sub.scscf_name = "sip:scscf.ims.mnc010.mcc404.3gppnetwork.org";
    registry_[impu] = sub;

    Logger::scscf("S-CSCF: registration stored ✓ — IMPU=" + impu);

    // ── Step 4: Send 200 OK back to P-CSCF → UE ──────────────
    sendSipResponse(SipMsgType::SIP_200_OK, "reg-" + impu, "REGISTER");
    Logger::scscf("S-CSCF: → SIP 200 OK — registration complete");
    Logger::ie_field("  Expires: 3600 (UE must re-register before this expires)");
    Logger::ie_field("  P-Associated-URI: tel:+919... (associated phone number)");
}

void ScscfNode::handleInvite(const std::vector<uint8_t>& payload) {
    std::string from, to, call_id, sdp;
    MessageReader r(payload);
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

    Logger::scscf("S-CSCF: ← SIP INVITE from P-CSCF");
    Logger::ie_field("  From: " + from);
    Logger::ie_field("  To:   " + to);
    Logger::ie_field("  Call-ID: " + call_id);
    Logger::ie_field("  SDP offer: " + sdp);

    // ── Send 100 Trying immediately ───────────────────────────
    Logger::scscf("S-CSCF: → SIP 100 Trying to P-CSCF");
    sendSipResponse(SipMsgType::SIP_100_TRYING, call_id, "INVITE");

    // ── Invoke MTAS via ISC ───────────────────────────────────
    invokeMtas(from, call_id, sdp);

    // ── Route INVITE to terminating side ─────────────────────
    Logger::scscf("S-CSCF: REAL: looks up called party's S-CSCF from HSS");
    Logger::scscf("S-CSCF: REAL: routes INVITE to terminating P-CSCF → UE-B");
    Logger::scscf("S-CSCF: SIM: simulating UE-B ringing...");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // ── 180 Ringing ──────────────────────────────────────────
    Logger::scscf("S-CSCF: → SIP 180 Ringing (UE-B is alerting)");
    sendSipResponse(SipMsgType::SIP_180_RINGING, call_id, "INVITE");

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // ── 200 OK with SDP answer ───────────────────────────────
    std::string sdp_answer = "audio:50000/AMR-WB/16000,video:50002/H264/90000";
    Logger::scscf("S-CSCF: → SIP 200 OK (UE-B answered)");
    Logger::ie_field("  SDP answer: " + sdp_answer);
    Logger::ie_field("  Codec negotiated: AMR-WB (Adaptive Multi-Rate Wideband) — HD Voice");
    Logger::ie_field("  RTP ports: audio=50000, video=50002");
    Logger::scscf("S-CSCF: REAL: SDP negotiation selects best codec both UEs support");
    Logger::scscf("S-CSCF: REAL: AMR-WB at 12.65kbps gives HD voice quality (4G VoLTE standard)");
    sendSipResponse(SipMsgType::SIP_200_OK, call_id, "INVITE", sdp_answer);
}

void ScscfNode::handleBye(const std::vector<uint8_t>& /*payload*/) {
    Logger::scscf("S-CSCF: ← SIP BYE — call terminated");
    Logger::scscf("S-CSCF: → SIP 200 OK");
    Logger::scscf("S-CSCF: REAL: notifies MTAS → MTAS updates CDR (charging record)");
    Logger::scscf("S-CSCF: REAL: P-CSCF sends Rx STR to PCRF → QCI=1 bearer released");
    sendSipResponse(SipMsgType::SIP_200_OK, "bye", "BYE");
}

void ScscfNode::invokeMtas(const std::string& /*impu*/, const std::string& /*call_id*/,
                            const std::string& /*sdp*/) {
    Logger::scscf("S-CSCF: → ISC: INVITE to MTAS (Ericsson Application Server)");
    Logger::ie_field("  MTAS role in this call:");
    Logger::ie_field("  1. OIP (Originating Identity Presentation): verify CLI allowed");
    Logger::ie_field("  2. Check call barring: is outgoing international call allowed?");
    Logger::ie_field("  3. MMTEL service: is call forking needed? (ring multiple devices)");
    Logger::ie_field("  4. Codec policy: enforce AMR-WB for VoLTE quality");
    Logger::ie_field("  5. Start CDR (Charging Data Record) generation");
    Logger::scscf("S-CSCF: MTAS → ISC → S-CSCF: continue routing (no barring applied)");
    Logger::scscf("S-CSCF: REAL: if call barring active → MTAS returns 603 Decline");
    Logger::scscf("S-CSCF: REAL: MTAS can also inject itself into media path (e.g., for recording)");
}

void ScscfNode::sendCxSAR(const std::string& impu, const std::string& impi) {
    MessageWriter w(static_cast<MessageType>(static_cast<uint16_t>(SipMsgType::DIA_CX_SAR)),
                    next_seq_++);
    w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::CX_IMPU)), impu);
    w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::CX_IMPI)), impi);
    hss_conn_.sendFrame(w.frame());
}

bool ScscfNode::waitCxSAA(std::string& profile) {
    if (!hss_conn_.hasData(3000)) return false;
    std::vector<uint8_t> payload;
    if (!hss_conn_.recvFrame(payload)) return false;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        if (tag == static_cast<Tag>(static_cast<uint16_t>(SipTag::CX_SCSCF_NAME)))
            profile = r.readStr();
        else r.skip();
    }
    return true;
}

void ScscfNode::sendSipResponse(SipMsgType type, const std::string& call_id,
                                  const std::string& reason, const std::string& sdp) {
    MessageWriter w(static_cast<MessageType>(static_cast<uint16_t>(type)), next_seq_++);
    w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CALL_ID)), call_id);
    w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_REASON)),  reason);
    if (!sdp.empty())
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_SDP)), sdp);
    pcscf_conn_.sendFrame(w.frame());
}
