#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>
#include <string>
#include <sstream>

#include "common/logger.h"
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "ims/sip.h"
#include "ims/pcscf_node.h"
#include "ims/scscf_node.h"
#include "ims/ims_hss.h"

// ============================================================
// IMS / VoLTE SIMULATOR — Main
//
// ARCHITECTURE:
//
//   [UE] ──SIP:5060──► [P-CSCF] ──SIP:5070──► [S-CSCF + MTAS]
//                                                    │
//                                               Cx:3870
//                                                    │
//                                               [IMS-HSS]
//
// CONNECTION TO 4G EPC (mme_sim):
//   1. UE first does 4G Attach via mme_sim → gets IP (10.0.0.1)
//   2. UE then does IMS Registration via mme_ims → P-CSCF → S-CSCF
//   3. VoLTE call: SIP INVITE → MTAS service logic → media via QCI=1 bearer
//
// THREADS:
//   ims_hss_th → ImsHssNode::run()   (Cx interface, port 3870)
//   scscf_th   → ScscfNode::run()    (SIP + MTAS, port 5070)
//   pcscf_th   → PcscfNode::run()    (SIP proxy, port 5060)
//   main       → UE simulator (connects to P-CSCF, sends SIP)
//
// COMMANDS:
//   REGISTER   → IMS registration flow
//   CALL       → VoLTE call setup
//   BYE        → end call
//   QUIT       → shutdown
// ============================================================

static std::atomic<bool>* g_stop = nullptr;
static void sig_handler(int) { if(g_stop) g_stop->store(true); }

// ── Simulate UE sending SIP messages ────────────────────────
class UeSimulator {
public:
    UeSimulator() : seq_(1) {}

    bool connect() {
        Logger::sys("[UE] connecting to P-CSCF on port 5060...");
        for (int i = 0; i < 30; ++i) {
            try {
                conn_ = Socket::connectTo("127.0.0.1", 5060);
                Logger::sys("[UE] connected to P-CSCF ✓");
                return true;
            } catch (...) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
        return false;
    }

    void sendRegister() {
        Logger::sys("");
        Logger::sys("════════════════════════════════════════════════════");
        Logger::sys("  IMS REGISTRATION FLOW");
        Logger::sys("  UE → P-CSCF → S-CSCF → HSS (Cx SAR/SAA) → 200 OK");
        Logger::sys("════════════════════════════════════════════════════");
        Logger::sys("[UE] → SIP REGISTER [RFC 3261 §10]");
        Logger::ie_field("  From:    sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        Logger::ie_field("  To:      sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        Logger::ie_field("  IMPU:    sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        Logger::ie_field("  IMPI:    919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        Logger::ie_field("  Contact: sip:ue@10.0.0.1:5060  (UE's IP from 4G attach!)");
        Logger::ie_field("  Expires: 3600");
        Logger::sys("[UE] REAL: adds Authorization header with IMS-AKA credentials");

        MessageWriter w(static_cast<MessageType>(static_cast<uint16_t>(SipMsgType::SIP_REGISTER)),
                        seq_++);
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_FROM)),
                   "sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_IMPU)),
                   "sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_IMPI)),
                   "919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CONTACT)),
                   "sip:ue@10.0.0.1:5060");
        conn_.sendFrame(w.frame());
    }

    void sendInvite(const std::string& called) {
        Logger::sys("");
        Logger::sys("════════════════════════════════════════════════════");
        Logger::sys("  VoLTE CALL SETUP FLOW");
        Logger::sys("  UE-A → P-CSCF → S-CSCF → MTAS → UE-B");
        Logger::sys("  On media confirm → Rx AAR → PCRF → QCI=1 bearer");
        Logger::sys("════════════════════════════════════════════════════");
        Logger::sys("[UE] → SIP INVITE [RFC 3261 §13]");
        Logger::ie_field("  From:    sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        Logger::ie_field("  To:      sip:" + called);
        Logger::ie_field("  Call-ID: call-abc-123");
        Logger::ie_field("  SDP offer: audio:50000/AMR-WB,video:50002/H264");
        Logger::sys("[UE] REAL: SDP contains: RTP port, codec list, bandwidth, direction");
        Logger::sys("[UE] REAL: P-Preferred-Identity header for CLI presentation");

        MessageWriter w(static_cast<MessageType>(static_cast<uint16_t>(SipMsgType::SIP_INVITE)),
                        seq_++);
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_FROM)),
                   "sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org");
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_TO)),
                   "sip:" + called);
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CALL_ID)),
                   "call-abc-123");
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_SDP)),
                   "audio:50000/AMR-WB,video:50002/H264");
        conn_.sendFrame(w.frame());
    }

    void sendBye() {
        Logger::sys("[UE] → SIP BYE — ending call");
        MessageWriter w(static_cast<MessageType>(static_cast<uint16_t>(SipMsgType::SIP_BYE)),
                        seq_++);
        w.writeStr(static_cast<Tag>(static_cast<uint16_t>(SipTag::SIP_CALL_ID)), "call-abc-123");
        conn_.sendFrame(w.frame());
    }

    bool hasResponse(int timeout_ms) { return conn_.hasData(timeout_ms); }
    std::vector<uint8_t> recvResponse() {
        std::vector<uint8_t> p; conn_.recvFrame(p); return p;
    }

private:
    Socket   conn_;
    uint32_t seq_;
};

int main() {
    Logger::sys("╔══════════════════════════════════════════════════════════╗");
    Logger::sys("║  IMS / VoLTE Simulator — Ericsson MTAS Interview Prep    ║");
    Logger::sys("║  Nodes: IMS-HSS(3870) S-CSCF+MTAS(5070) P-CSCF(5060)   ║");
    Logger::sys("╚══════════════════════════════════════════════════════════╝");
    Logger::sys("");
    Logger::sys("PREREQUISITE: UE must be REGISTERED in 4G EPC first (mme_sim).");
    Logger::sys("  UE gets IP=10.0.0.1 from P-GW → uses it as IMS Contact address.");
    Logger::sys("  This simulator shows what happens AFTER the 4G data bearer is up.");
    Logger::sys("");
    Logger::sys("Commands: REGISTER  CALL  BYE  CONF  WAIT  BARR  QUIT");
    Logger::sys("──────────────────────────────────────────────────────────");

    std::atomic<bool> stop{false};
    std::atomic<bool> ims_hss_ready{false}, scscf_ready{false}, pcscf_ready{false};

    g_stop = &stop;
    std::signal(SIGINT, sig_handler);

    ImsHssNode ims_hss(stop, ims_hss_ready);
    ScscfNode  scscf(stop, scscf_ready, ims_hss_ready);
    PcscfNode  pcscf(stop, pcscf_ready);

    std::thread hss_th  ([&]{ ims_hss.run(); });
    std::thread scscf_th([&]{ scscf.run();   });
    std::thread pcscf_th([&]{ pcscf.run();   });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // UE simulator (runs on main thread)
    UeSimulator ue;
    if (!ue.connect()) {
        Logger::warn("UE", "cannot connect to P-CSCF");
        stop.store(true);
    }

    std::string line;
    std::cout << "\nims-sim> " << std::flush;

    while (!stop.load() && std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << "ims-sim> " << std::flush; continue; }

        std::istringstream iss(line); std::string cmd; iss >> cmd;
        for(auto& c:cmd) c=char(std::toupper(unsigned(c)));

        if (cmd=="QUIT") { break; }
        else if (cmd=="REGISTER") {
            ue.sendRegister();
            // wait for 200 OK
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            Logger::sys("[UE] ← SIP 200 OK — IMS registration complete ✓");
            Logger::sys("[UE] UE is now registered in IMS. Ready to make VoLTE calls.");
        }
        else if (cmd=="CALL") {
            std::string num = "+919000000002@ims.mnc010.mcc404.3gppnetwork.org";
            iss >> num;
            ue.sendInvite(num);
            std::this_thread::sleep_for(std::chrono::milliseconds(1200));
            Logger::sys("[UE] ← SIP 200 OK — call established ✓");
            Logger::sys("[UE] RTP media flowing on QCI=1 dedicated bearer");
            Logger::sys("[UE] Voice codec: AMR-WB 12.65kbps (HD Voice)");
        }
        else if (cmd=="BYE") {
            ue.sendBye();
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            Logger::sys("[UE] ← SIP 200 OK — call ended");
            Logger::sys("[UE] QCI=1 bearer released via Rx STR → PCRF → P-GW");
        }
        else if (cmd=="CONF") {
            Logger::sys("");
            Logger::sys("════════════════════════════════════════════════════");
            Logger::sys("  CONFERENCE CALL FLOW (add 3rd party)");
            Logger::sys("  UE-A in call → adds UE-C via MRFC/MRFP");
            Logger::sys("════════════════════════════════════════════════════");
            Logger::sys("[UE-A] → SIP re-INVITE with conference URI");
            Logger::sys("  S-CSCF → MTAS: ISC trigger for CONF service");
            Logger::sys("  MTAS  → MRFC: SIP INVITE on Mr interface (port 5060)");
            Logger::sys("          'Create conference bridge'");
            Logger::sys("  MRFC  → MRFP: H.248/Megaco (port 2944)");
            Logger::sys("          'Allocate mixing endpoint, 3 participants'");
            Logger::sys("  MRFP allocates: conf-URI = sip:conf-123@mrfc.ims");
            Logger::sys("  MTAS sends INVITE to UE-B and UE-C with conf-URI");
            Logger::sys("  All 3 UEs send RTP → MRFP → mixed audio → each UE");
            Logger::sys("[CONF] ✓ 3-party conference established");
            Logger::sys("  Each hears the other two — MRFP does the mixing");
            Logger::sys("  INTERVIEW: MRFC=controller(SIP), MRFP=processor(H.248+DSP)");
        }
        else if (cmd=="WAIT") {
            Logger::sys("");
            Logger::sys("════════════════════════════════════════════════════");
            Logger::sys("  CALL WAITING FLOW");
            Logger::sys("  UE-A in call with UE-B, UE-C calls UE-A");
            Logger::sys("════════════════════════════════════════════════════");
            Logger::sys("[UE-C] → SIP INVITE to UE-A");
            Logger::sys("  S-CSCF → MTAS: ISC INVITE (iFC trigger: incoming call)");
            Logger::sys("  MTAS checks: is UE-A currently in an active dialog?");
            Logger::sys("  YES → UE-A has active call (stored dialog state)");
            Logger::sys("  MTAS applies Call Waiting service:");
            Logger::sys("  MTAS → S-CSCF: 180 Ringing (UE-A will be notified)");
            Logger::sys("  MTAS → UE-A: re-INVITE with call-waiting indication");
            Logger::sys("  UE-A: 🔔 Beep heard — '2nd call from UE-C'");
            Logger::sys("  UE-A OPTIONS:");
            Logger::sys("    a) Accept: MTAS puts UE-B on HOLD (re-INVITE with a=inactive SDP)");
            Logger::sys("               UE-A answers UE-C — two call legs managed by MTAS");
            Logger::sys("    b) Reject: MTAS sends 486 Busy to UE-C");
            Logger::sys("    c) Timeout: MTAS forwards to voicemail after 20s");
            Logger::sys("[WAIT] SIM: UE-A accepts → UE-B on hold, UE-C answered ✓");
        }
        else if (cmd=="BARR") {
            Logger::sys("");
            Logger::sys("════════════════════════════════════════════════════");
            Logger::sys("  CALL BARRING FLOW");
            Logger::sys("  UE tries to call international number (barred)");
            Logger::sys("════════════════════════════════════════════════════");
            Logger::sys("[UE] → SIP INVITE To: +44-xxx (UK number)");
            Logger::sys("  P-CSCF → S-CSCF → MTAS: ISC INVITE");
            Logger::sys("  MTAS checks barring rules for this subscriber:");
            Logger::sys("  Rule: OIB (Outgoing International Barring) = ACTIVE");
            Logger::sys("  +44 = UK = International prefix → BARRED");
            Logger::sys("  MTAS → S-CSCF: 603 Decline");
            Logger::sys("  S-CSCF → P-CSCF → UE: 603 Decline");
            Logger::sys("[BARR] ✗ Call rejected — UE displays 'Call Barred'");
            Logger::sys("  Barring types: OIB, OIBH, BAIC, BIC-Roam");
            Logger::sys("  All managed by MTAS service logic");
        }
        else {
            Logger::sys("Unknown. Try: REGISTER, CALL, BYE, CONF, WAIT, BARR, QUIT");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!stop.load()) std::cout << "\nims-sim> " << std::flush;
    }

    stop.store(true);
    Logger::sys("Shutting down IMS nodes...");
    hss_th.join(); scscf_th.join(); pcscf_th.join();
    Logger::sys("Done.");
    return 0;
}
