#include <iostream>
#include <string>
#include <map>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>
#include <sstream>
#include <cstring>

#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "common/pcap_writer.h"
#include "ims/sip.h"
#include "ims/sip_text.h"

// ============================================================
// UE SIMULATOR — run as: ./ue_sim A  or  ./ue_sim B  or  ./ue_sim C
//
// Each terminal represents ONE phone.
// All three connect to the shared IMS server (./ims_server).
//
// COMMANDS:
//   REG          → SIP REGISTER with IMS
//   CALL B       → INVITE sip:+919000000002 (or A, C)
//   ACCEPT       → answer incoming call (send 200 OK)
//   REJECT       → reject incoming call (send 486 Busy)
//   HOLD         → re-INVITE with SDP a=sendonly
//   RESUME       → re-INVITE with SDP a=sendrecv
//   CONF C       → add UE-C to conference (re-INVITE with conf URI)
//   BYE          → end call
//   STATUS       → show current state
//   QUIT
// ============================================================

static std::atomic<bool> g_stop{false};
static void sig_handler(int) { g_stop.store(true); }

struct UeConfig {
    std::string label;    // "UE-A"
    std::string impu;
    std::string impi;
    std::string ip;       // 4G IP from P-GW
    int         rtp_port;
};

static std::map<std::string, UeConfig> UE_CONFIGS = {
    {"A", {"UE-A", IMPU_A, "919000000001@" + IMS_DOMAIN, IP_UE_A, 50000}},
    {"B", {"UE-B", IMPU_B, "919000000002@" + IMS_DOMAIN, IP_UE_B, 60000}},
    {"C", {"UE-C", IMPU_C, "919000000003@" + IMS_DOMAIN, IP_UE_C, 70000}},
};

static const std::map<std::string, std::string> ID_TO_IMPU = {
    {"A", IMPU_A}, {"B", IMPU_B}, {"C", IMPU_C}
};

enum class UeState { IDLE, REGISTERING, REGISTERED, CALLING, RINGING, IN_CALL, ON_HOLD };
static const char* state_str(UeState s) {
    switch(s) {
        case UeState::IDLE:        return "IDLE";
        case UeState::REGISTERING: return "REGISTERING";
        case UeState::REGISTERED:  return "REGISTERED";
        case UeState::CALLING:     return "CALLING";
        case UeState::RINGING:     return "RINGING (incoming)";
        case UeState::IN_CALL:     return "IN-CALL";
        case UeState::ON_HOLD:     return "ON-HOLD";
        default: return "?";
    }
}

class UeSimulator {
public:
    explicit UeSimulator(const UeConfig& cfg) : cfg_(cfg) {}

    bool connect() {
        for (int i = 0; i < 30; ++i) {
            try { conn_ = Socket::connectTo("127.0.0.1", 5060); return true; }
            catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }
        }
        return false;
    }

    void startReceiveThread() {
        rx_thread_ = std::thread([this]{ receiveLoop(); });
    }

    void stopReceiveThread() {
        if (rx_thread_.joinable()) rx_thread_.join();
    }

    // ── Commands ─────────────────────────────────────────────
    void doRegister() {
        if (state_ == UeState::IN_CALL) { print("Cannot register while in call"); return; }
        print("→ SIP REGISTER");
        print("  IMPU: " + cfg_.impu);
        print("  Contact: sip:ue@" + cfg_.ip + ":5060  ← 4G IP from EPC P-GW!");
        print("  P-Access-Network-Info: 3GPP-E-UTRAN-FDD");
        state_ = UeState::REGISTERING;

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_REGISTER)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_IMPU)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_IMPI)),    cfg_.impi);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CONTACT)), "sip:ue@" + cfg_.ip + ":5060");
        conn_.sendFrame(w.frame());

        // Write real SIP to PCAP
        PcapWriter::instance().writeSIP(
            SipText::buildRegister(cfg_.impu, cfg_.ip, next_seq_),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
    }

    void doCall(const std::string& target_id) {
        if (state_ != UeState::REGISTERED) { print("Not registered. Run REG first."); return; }
        auto it = ID_TO_IMPU.find(target_id);
        if (it == ID_TO_IMPU.end()) { print("Unknown target: " + target_id); return; }
        current_call_id_ = "call-" + cfg_.label.back() + target_id + "-" + std::to_string(next_seq_);
        callee_impu_ = it->second;

        print("→ SIP INVITE  (VoLTE call)");
        print("  To:      " + callee_impu_);
        print("  Call-ID: " + current_call_id_);
        print("  SDP:     audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB, video:" + std::to_string(cfg_.rtp_port+2) + "/H264");
        print("  P-Preferred-Identity: " + cfg_.impu + "  (CLI)");
        state_ = UeState::CALLING;

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      callee_impu_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
                   "audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB,video:" + std::to_string(cfg_.rtp_port+2) + "/H264");
        conn_.sendFrame(w.frame());

        PcapWriter::instance().writeSIP(
            SipText::buildInvite(cfg_.impu, callee_impu_, cfg_.ip, current_call_id_, next_seq_),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
    }

    void doAccept() {
        if (state_ != UeState::RINGING) { print("No incoming call to accept."); return; }
        print("→ SIP 200 OK  (accepting call)");
        print("  SDP answer: audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB  (HD Voice!)");
        print("  Codec negotiated: AMR-WB/16000 = 16kHz HD Voice");
        state_ = UeState::IN_CALL;

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_200_OK)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_REASON)),  "INVITE");
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
                   "audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB/16000");
        conn_.sendFrame(w.frame());

        PcapWriter::instance().writeSIP(
            SipText::build200Invite(caller_impu_, cfg_.impu, cfg_.ip, current_call_id_, next_seq_),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);

        print("✓ Call accepted — QCI=1 bearer will be created by P-CSCF→PCRF");
        print("  RTP voice path: " + cfg_.ip + ":" + std::to_string(cfg_.rtp_port) + " ←→ peer");
    }

    void doReject() {
        if (state_ != UeState::RINGING) { print("No incoming call to reject."); return; }
        print("→ SIP 486 Busy Here  (rejecting call)");
        state_ = UeState::REGISTERED;
        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_CANCEL)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        conn_.sendFrame(w.frame());
    }

    void doHold() {
        if (state_ != UeState::IN_CALL) { print("Not in a call."); return; }
        print("→ SIP re-INVITE  (HOLD)");
        print("  SDP: a=sendonly  (you stop receiving, still send)");
        print("  INTERVIEW: a=sendonly=hold, a=inactive=full hold, a=sendrecv=active");
        state_ = UeState::ON_HOLD;

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      callee_impu_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
                   "audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB;a=sendonly");
        conn_.sendFrame(w.frame());
        PcapWriter::instance().writeSIP(
            SipText::buildReInvite(cfg_.impu, callee_impu_, cfg_.ip, current_call_id_, next_seq_,
                "v=0\r\no=ue 1 1 IN IP4 " + cfg_.ip + "\r\ns=-\r\nt=0 0\r\n"
                "m=audio " + std::to_string(cfg_.rtp_port) + " RTP/AVP 98\r\n"
                "a=rtpmap:98 AMR-WB/16000\r\na=sendonly\r\n"),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
        print("  Callee hears hold music — P-CSCF updates Rx (reduces bearer to one-way)");
    }

    void doResume() {
        if (state_ != UeState::ON_HOLD) { print("Not on hold."); return; }
        print("→ SIP re-INVITE  (RESUME)");
        print("  SDP: a=sendrecv  (restore bidirectional voice)");
        state_ = UeState::IN_CALL;

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      callee_impu_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
                   "audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB;a=sendrecv");
        conn_.sendFrame(w.frame());
        PcapWriter::instance().writeSIP(
            SipText::buildReInvite(cfg_.impu, callee_impu_, cfg_.ip, current_call_id_, next_seq_,
                "v=0\r\no=ue 1 1 IN IP4 " + cfg_.ip + "\r\ns=-\r\nt=0 0\r\n"
                "m=audio " + std::to_string(cfg_.rtp_port) + " RTP/AVP 98\r\n"
                "a=rtpmap:98 AMR-WB/16000\r\na=sendrecv\r\n"),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
    }

    void doConference(const std::string& third_id) {
        if (state_ != UeState::IN_CALL) { print("Must be in a call first."); return; }
        auto it = ID_TO_IMPU.find(third_id);
        if (it == ID_TO_IMPU.end()) { print("Unknown UE: " + third_id); return; }

        std::string conf_uri = "sip:conf-" + std::to_string(next_seq_) + "@mrfc." + IMS_DOMAIN;
        print("→ SIP re-INVITE  (CONFERENCE — adding " + third_id + ")");
        print("  Conference URI: " + conf_uri);
        print("  S-CSCF → MTAS → MRFC (Mr/SIP) → MRFP (H.248/Megaco)");
        print("  MRFP allocates 3-way audio mixing bridge");

        // KEY: set To = UE-C's IMPU so S-CSCF knows who to add to conference
        // S-CSCF reads this To: to send INVITE to UE-C
        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      it->second); // UE-C's IMPU
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),     conf_uri + ";a=sendrecv;conf");
        conn_.sendFrame(w.frame());

        PcapWriter::instance().writeSIP(
            SipText::buildInvite(cfg_.impu, it->second, cfg_.ip,
                                 "conf-" + std::to_string(next_seq_), next_seq_+1),
            ue_ip_num(), 5060, PcapWriter::IP_SCSCF, 5060);
    }

    // ── UPDATE (RFC 3311) ─────────────────────────────────────
    // SIP UPDATE modifies session WITHOUT changing dialog state.
    // Used for: QoS preconditions, codec change, hold/resume.
    // Different from re-INVITE: UPDATE doesn't need ACK.
    void doUpdate(const std::string& reason = "qos") {
        if (state_ != UeState::IN_CALL && state_ != UeState::ON_HOLD) {
            print("Not in a call."); return;
        }
        print("→ SIP UPDATE  (RFC 3311)");
        if (reason == "qos") {
            print("  Purpose: QoS precondition satisfied — signalling bearer ready");
            print("  SDP:     a=curr:qos local sendrecv");
            print("           a=curr:qos remote sendrecv");
            print("           a=des:qos mandatory local sendrecv");
            print("  REAL:    UE sends UPDATE after QCI=1 bearer confirmed by eNB");
            print("  REAL:    callee checks its QoS too, responds 200 OK with same SDP");
            print("  MTAS:    updates CDR with QoS confirmation timestamp");
        } else if (reason == "codec") {
            print("  Purpose: Codec renegotiation during active call");
            print("  SDP:     switching from AMR-WB to AMR-NB (weaker signal)");
            print("  REAL:    adaptive codec switch based on radio conditions");
            print("  MTAS:    updates CDR codec field");
        }
        print("  Key diff vs re-INVITE: UPDATE doesn't disrupt dialog state");
        print("  Key diff vs re-INVITE: no ACK needed after 200 OK to UPDATE");

        // S-CSCF will handle this as a mid-dialog request
        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
        // Using INVITE type for simplicity (UPDATE would need new SipMsgType)
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      callee_impu_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
                   "audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB;"
                   "a=curr:qos local sendrecv;a=des:qos mandatory local sendrecv");
        conn_.sendFrame(w.frame());
        print("  UPDATE sent — waiting for 200 OK from callee");
    }

    void doVideo() {
        if (state_ != UeState::IN_CALL) { print("Not in a call."); return; }
        print("→ SIP re-INVITE  (add VIDEO to voice call)");
        print("  SDP: m=audio 50000 AMR-WB  +  m=video 50002 H264/90000");
        print("  MTAS: checks video policy — H264 approved");
        print("  P-CSCF: Rx AAR update — add video component to bearer");
        print("  PCRF: installs QCI=2 video bearer alongside QCI=1 voice");

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      callee_impu_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
            "audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB;"
            "video:" + std::to_string(cfg_.rtp_port+2) + "/H264/90000;a=sendrecv");
        conn_.sendFrame(w.frame());
        PcapWriter::instance().writeSIP(
            SipText::buildReInvite(cfg_.impu, callee_impu_, cfg_.ip, current_call_id_, next_seq_,
                "v=0\r\no=ue 1 1 IN IP4 " + cfg_.ip + "\r\ns=-\r\nt=0 0\r\n"
                "m=audio " + std::to_string(cfg_.rtp_port) + " RTP/AVP 98\r\n"
                "a=rtpmap:98 AMR-WB/16000\r\na=sendrecv\r\n"
                "m=video " + std::to_string(cfg_.rtp_port+2) + " RTP/AVP 100\r\n"
                "a=rtpmap:100 H264/90000\r\na=sendrecv\r\n"),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
    }

    void doVoice() {
        if (state_ != UeState::IN_CALL) { print("Not in a call."); return; }
        print("→ SIP re-INVITE  (drop video, voice only)");
        print("  SDP: m=audio 50000 AMR-WB  +  m=video 0 H264 (port=0 = remove)");
        print("  P-CSCF: Rx STR for video component → QCI=2 bearer released");

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_INVITE)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      callee_impu_);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_SDP)),
            "audio:" + std::to_string(cfg_.rtp_port) + "/AMR-WB;"
            "video:0/H264/90000;port=0;a=sendrecv");
        conn_.sendFrame(w.frame());
        PcapWriter::instance().writeSIP(
            SipText::buildReInvite(cfg_.impu, callee_impu_, cfg_.ip, current_call_id_, next_seq_,
                "v=0\r\no=ue 1 1 IN IP4 " + cfg_.ip + "\r\ns=-\r\nt=0 0\r\n"
                "m=audio " + std::to_string(cfg_.rtp_port) + " RTP/AVP 98\r\n"
                "a=rtpmap:98 AMR-WB/16000\r\na=sendrecv\r\n"
                "m=video 0 RTP/AVP 100\r\na=rtpmap:100 H264/90000\r\n"),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
    }

    void doBye() {
        if (state_ != UeState::IN_CALL && state_ != UeState::ON_HOLD) {
            print("Not in a call."); return;
        }
        print("→ SIP BYE  (ending call)");
        print("  P-CSCF will send Rx STR → PCRF → QCI=1 bearer released");
        state_ = UeState::REGISTERED;

        MessageWriter w(static_cast<MessageType>(uint16_t(SipMsgType::SIP_BYE)), next_seq_++);
        w.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
        conn_.sendFrame(w.frame());

        PcapWriter::instance().writeSIP(
            SipText::buildBye(cfg_.impu, callee_impu_, current_call_id_, next_seq_),
            ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
        print("  Call ended ✓");
    }

    void doStatus() {
        print("=== " + cfg_.label + " STATUS ===");
        print("  IMPU:    " + cfg_.impu);
        print("  IP:      " + cfg_.ip + "  (4G P-GW allocated)");
        print("  State:   " + std::string(state_str(state_)));
        if (state_ == UeState::IN_CALL || state_ == UeState::ON_HOLD)
            print("  Call-ID: " + current_call_id_);
    }

private:
    // ── Receive loop — runs on background thread ──────────────
    void receiveLoop() {
        while (!g_stop.load()) {
            if (!conn_.hasData(100)) continue;
            std::vector<uint8_t> payload;
            if (!conn_.recvFrame(payload)) {
                print("[rx] connection lost — P-CSCF disconnected");
                break;
            }
            print("[rx] got " + std::to_string(payload.size()) + " bytes from P-CSCF");
            handleIncoming(payload);
        }
    }

    void handleIncoming(const std::vector<uint8_t>& payload) {
        MessageReader r(payload);
        auto type = static_cast<SipMsgType>(static_cast<uint16_t>(r.msgType()));

        std::string from, to, call_id, reason, sdp;
        MessageReader r2(payload);
        while (r2.hasMore()) {
            Tag tag; uint16_t len; if (!r2.peek(tag, len)) break;
            if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_FROM)))     from    = r2.readStr();
            else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_TO)))  to      = r2.readStr();
            else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID))) call_id = r2.readStr();
            else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_REASON))) reason = r2.readStr();
            else if (tag == static_cast<Tag>(uint16_t(SipTag::SIP_SDP))) sdp     = r2.readStr();
            else r2.skip();
        }

        switch (type) {
        case SipMsgType::SIP_200_OK:
            if (reason == "REGISTER") {
                state_ = UeState::REGISTERED;
                printBanner("✓ IMS REGISTRATION COMPLETE", Logger::CLR_ENB);
                print("  P-Associated-URI: tel:+91" + cfg_.label);
                print("  Service-Route: sip:scscf." + IMS_DOMAIN);
                print("  Expires: 3600s  (re-REGISTER before expiry)");
                print("  MTAS enabled: OIP/OIR, call waiting, forwarding");
            } else if (reason == "INVITE" || reason.empty()) {
                state_ = UeState::IN_CALL;
                printBanner("✓ CALL CONNECTED  — RTP flowing", Logger::CLR_ENB);
                print("  SDP answer: " + (sdp.empty() ? "AMR-WB/16000 (HD Voice)" : sdp));
                print("  Codec: AMR-WB 12.65kbps — voice sounds like in the room");
                print("  QCI=1 dedicated bearer active (via P-CSCF → Rx AAR → PCRF)");
                print("  Type HOLD / BYE / CONF C");

                print("→ SIP ACK  (completing 3-way handshake)");
                MessageWriter ackw(static_cast<MessageType>(uint16_t(SipMsgType::SIP_ACK)), next_seq_++);
                ackw.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_CALL_ID)), current_call_id_);
                ackw.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_FROM)),    cfg_.impu);
                ackw.writeStr(static_cast<Tag>(uint16_t(SipTag::SIP_TO)),      callee_impu_);
                conn_.sendFrame(ackw.frame());
                PcapWriter::instance().writeSIP(
                    SipText::buildAck(cfg_.impu, callee_impu_, current_call_id_, next_seq_),
                    ue_ip_num(), 5060, PcapWriter::IP_PCSCF, 5060);
            } else if (reason == "re-INVITE-HOLD") {
                printBanner("⏸  OTHER SIDE PUT ON HOLD", Logger::CLR_SGW);
                print("  SDP: a=sendonly — you hear hold music");
            } else if (reason == "re-INVITE-RESUME") {
                state_ = UeState::IN_CALL;
                printBanner("▶  CALL RESUMED", Logger::CLR_ENB);
            } else if (reason == "CONFERENCE") {
                printBanner("👥 IN CONFERENCE CALL", Logger::CLR_MTAS);
                print("  3-party: MRFP mixing audio streams");
                print("  Each hears the other two via MRFP bridge");
                print("  Type CONF LEAVE (BYE) to leave conference");
            } else if (reason == "VIDEO-ADD") {
                printBanner("📹 VIDEO CALL ACTIVE", Logger::CLR_PCSCF);
                print("  Audio: AMR-WB (QCI=1) + Video: H264/90000 (QCI=2)");
                print("  Type VOICE to drop video and keep voice only");
            } else if (reason == "VIDEO-REMOVE") {
                printBanner("🎤 VOICE ONLY (video dropped)", Logger::CLR_ENB);
                print("  QCI=2 video bearer released");
                print("  QCI=1 voice bearer still active");
            } else if (reason == "BYE") {
                state_ = UeState::REGISTERED;
                printBanner("✓ CALL ENDED", Logger::CLR_SYS);
                print("  QCI=1 bearer released via Rx STR → PCRF");
            }
            break;

        case SipMsgType::SIP_INVITE: {
            bool is_hold = (sdp.find("sendonly") != std::string::npos ||
                            sdp.find("inactive") != std::string::npos);
            bool is_conf = (sdp.find("conf") != std::string::npos);
            bool is_reinvite = (!current_call_id_.empty() && call_id == current_call_id_);

            if (is_hold && is_reinvite) {
                // ── re-INVITE: HOLD ──────────────────────────
                printBanner("⏸  PUT ON HOLD", Logger::CLR_SGW);
                print("  Caller put you on hold");
                print("  SDP: a=sendonly (you still send, caller not receiving)");
                print("  Hold music playing...");
                print("  Wait for caller to RESUME");

            } else if (is_conf) {
                // ── Conference INVITE ─────────────────────────
                if (!call_id.empty()) current_call_id_ = call_id;
                if (!from.empty()) caller_impu_ = from;
                state_ = UeState::RINGING;
                printBanner("👥 CONFERENCE INVITE", Logger::CLR_MTAS);
                print("  From:      " + from + "  (adding you to conference)");
                print("  Call-ID:   " + call_id);
                print("  MRFC:      conference bridge ready");
                print("  MRFP:      3-party audio mixing active");
                print("  SDP:       " + sdp);
                print("  >>> Type ACCEPT to join conference");

            } else {
                // ── New incoming call ─────────────────────────
                if (!call_id.empty()) current_call_id_ = call_id;
                if (!from.empty()) caller_impu_ = from;
                state_ = UeState::RINGING;
                printBanner("📞 INCOMING CALL", Logger::CLR_PCSCF);
                print("  From:    " + from + "  (CLI — presented by MTAS OIP)");
                print("  Call-ID: " + call_id + "  (unique dialog ID)");
                print("  SDP:     " + (sdp.empty() ? "audio/AMR-WB + video/H264 offer" : sdp));
                print("  MTAS:    OIP verified, TIP presentation OK, CDR started");
                print("  >>> Type ACCEPT to answer  |  REJECT to decline");
            }
            PcapWriter::instance().writeSIP(
                SipText::buildInvite(from, cfg_.impu, "10.0.0.x", call_id, 1),
                PcapWriter::IP_PCSCF, 5060, ue_ip_num(), 5060);
            break;
        }

        case SipMsgType::SIP_100_TRYING:
            print("← 100 Trying  (IMS received INVITE, processing...)");
            print("  MTAS invoked via ISC — checking OIP, barring, forwarding");
            break;

        case SipMsgType::SIP_180_RINGING:
            print("← 180 Ringing  🔔");
            print("  To-tag added — SIP dialog established");
            print("  Callee's phone is ringing — you hear ringback");
            print("  PRACK sent for reliable provisional response (RFC 3262)");
            PcapWriter::instance().writeSIP(
                SipText::build180Ringing(cfg_.impu, callee_impu_, current_call_id_, 1),
                PcapWriter::IP_PCSCF, 5060, ue_ip_num(), 5060);
            break;

        case SipMsgType::SIP_ACK:
            print("← ACK  (3-way handshake complete — call fully established)");
            PcapWriter::instance().writeSIP(
                SipText::buildAck(caller_impu_, cfg_.impu, current_call_id_, 1),
                PcapWriter::IP_PCSCF, 5060, ue_ip_num(), 5060);
            break;

        default:
            print("← " + std::string(sip_type_str(type)) + "  (received)");
            break;
        }
        // Re-print prompt
        std::cout << "\n" << cfg_.label << "> " << std::flush;
    }

    void print(const std::string& msg) {
        std::lock_guard<std::mutex> lk(print_mtx_);
        std::cout << Logger::CLR_ENB << "  [" << cfg_.label << "] " << msg
                  << Logger::CLR_RESET << "\n";
    }

    void printBanner(const std::string& msg, const char* color) {
        std::lock_guard<std::mutex> lk(print_mtx_);
        std::cout << color
                  << "\n  ╔══════════════════════════════════════════╗\n"
                  << "  ║  " << cfg_.label << "  " << msg << "\n"
                  << "  ╚══════════════════════════════════════════╝\n"
                  << Logger::CLR_RESET;
    }

    uint32_t ue_ip_num() const {
        if (cfg_.label == "UE-A") return PcapWriter::IP_UE;
        if (cfg_.label == "UE-B") return PcapWriter::IP_UE_B;
        return PcapWriter::IP_UE_C;
    }

    UeConfig     cfg_;
    Socket       conn_;
    std::thread  rx_thread_;
    std::mutex   print_mtx_;

    std::atomic<UeState> state_{UeState::IDLE};
    uint32_t    next_seq_{1};
    std::string current_call_id_;
    std::string callee_impu_;
    std::string caller_impu_;
};

int main(int argc, char* argv[]) {
    if (argc < 2 || !UE_CONFIGS.count(std::string(1, char(std::toupper(unsigned(argv[1][0])))))) {
        std::cerr << "Usage: ./ue_sim A|B|C\n"
                  << "  Each terminal is one phone.\n"
                  << "  Run ./ims_server first in a separate terminal.\n";
        return 1;
    }

    std::string id(1, char(std::toupper(unsigned(argv[1][0]))));
    const UeConfig& cfg = UE_CONFIGS.at(id);

    std::cout << "\n"
              << "  +==========================================+\n"
              << "  |  " << cfg.label << " — " << cfg.impu << "\n"
              << "  |  IP: " << cfg.ip << "  (from 4G EPC attach)\n"
              << "  |  RTP port: " << cfg.rtp_port << "\n"
              << "  +==========================================+\n"
              << "  Commands: REG  CALL A|B|C  ACCEPT  REJECT\n"
              << "            HOLD  RESUME  UPDATE  VIDEO  VOICE  CONF C  BYE  STATUS  QUIT\n"
              << "  ----------------------------------------\n\n";

    std::signal(SIGINT, sig_handler);

    std::string pcap_file = "ims_" + id + "_capture.pcap";
    PcapWriter::instance().open(pcap_file);

    UeSimulator ue(cfg);
    if (!ue.connect()) {
        std::cerr << "Cannot connect to IMS server on port 5060.\n"
                  << "Start ./ims_server first!\n";
        return 1;
    }
    std::cout << "  Connected to IMS server ✓\n\n";

    ue.startReceiveThread();

    std::string line;
    std::cout << cfg.label << "> " << std::flush;

    while (!g_stop.load() && std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << cfg.label << "> " << std::flush; continue; }

        std::istringstream iss(line);
        std::string cmd; iss >> cmd;
        for (auto& c : cmd) c = char(std::toupper(unsigned(c)));

        if      (cmd == "QUIT")   break;
        else if (cmd == "REG")    ue.doRegister();
        else if (cmd == "STATUS") ue.doStatus();
        else if (cmd == "ACCEPT") ue.doAccept();
        else if (cmd == "REJECT") ue.doReject();
        else if (cmd == "HOLD")   ue.doHold();
        else if (cmd == "RESUME") ue.doResume();
        else if (cmd == "BYE")    ue.doBye();
        else if (cmd == "CALL") {
            std::string tgt; iss >> tgt;
            for (auto& c : tgt) c = char(std::toupper(unsigned(c)));
            ue.doCall(tgt);
        }
        else if (cmd == "CONF") {
            std::string tgt; iss >> tgt;
            for (auto& c : tgt) c = char(std::toupper(unsigned(c)));
            ue.doConference(tgt);
        }
        else if (cmd == "UPDATE") {
            std::string reason = "qos"; iss >> reason;
            ue.doUpdate(reason.empty() ? "qos" : reason);
        }
        else if (cmd == "VIDEO") ue.doVideo();
        else if (cmd == "VOICE") ue.doVoice();
        else std::cout << "  Unknown. Try: REG CALL ACCEPT REJECT HOLD RESUME VIDEO VOICE CONF BYE STATUS\n";

        if (!g_stop.load()) std::cout << "\n" << cfg.label << "> " << std::flush;
    }

    g_stop.store(true);
    ue.stopReceiveThread();
    PcapWriter::instance().close();
    Logger::shutdown();
    return 0;
}
