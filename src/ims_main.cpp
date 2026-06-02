#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>
#include <string>
#include <sstream>
#include <map>

#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/pcap_writer.h"
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "ims/sip.h"
#include "ims/sip_text.h"
#include "ims/pcscf_node.h"
#include "ims/scscf_node.h"
#include "ims/ims_hss.h"

// ============================================================
// IMS / VoLTE SIMULATOR — Three UE Terminals (A, B, C)
//
// CONNECTION TO 4G EPC:
//   UE first does 4G Attach via mme_sim:
//     UE-A gets IP 10.0.0.1  (from P-GW)
//     UE-B gets IP 10.0.0.2
//     UE-C gets IP 10.0.0.3
//   These IPs appear in the SIP Contact header below!
//   That's the concrete link between EPC and IMS.
//
//   When VoLTE call is established:
//     P-CSCF sends Diameter Rx AAR to PCRF
//     PCRF tells P-GW to create QCI=1 dedicated bearer
//     Voice (RTP/AMR-WB) flows on QCI=1 bearer
//
// NODES:
//   IMS-HSS  : Cx interface  port 3870
//   S-CSCF   : SIP registrar port 5070
//   P-CSCF   : SIP proxy     port 5060
//   MTAS     : co-located with S-CSCF (ISC interface)
//
// COMMANDS:
//   REG A|B|C|ALL  → IMS registration
//   CALL A B       → VoLTE call (A calls B)
//   CALL A C       → A calls C
//   CONF           → 3-party conference (A+B+C via MRFC/MRFP)
//   WAIT           → Call waiting scenario
//   BARR           → Call barring (OIB)
//   STATUS         → Show registered UEs
//   QUIT
// ============================================================

static std::atomic<bool>* g_stop = nullptr;
static void sig_handler(int) { if(g_stop) g_stop->store(true); }

// ── UE identity ───────────────────────────────────────────────
struct UeIdentity {
    std::string label;   // "UE-A"
    std::string impu;    // sip:+919...@ims.domain
    std::string ip;      // 4G P-GW allocated IP
    bool        registered{false};
    int         cseq{1};
};

static std::map<std::string, UeIdentity> g_ues = {
    {"A", {"UE-A", IMPU_A, IP_UE_A, false, 1}},
    {"B", {"UE-B", IMPU_B, IP_UE_B, false, 1}},
    {"C", {"UE-C", IMPU_C, IP_UE_C, false, 1}},
};

static int g_call_seq = 1;

// ── Helper: write real SIP text to PCAP so Wireshark shows "SIP" ──
static void pcapSip(const std::string& sip_text,
                    uint32_t src_ip, uint32_t dst_ip) {
    PcapWriter::instance().writeSIP(sip_text, src_ip, 5060, dst_ip, 5060);
}

// ── Registration flow ─────────────────────────────────────────
static void doRegister(const std::string& id) {
    auto& ue = g_ues[id];

    VLog::step(1, 4, "SIP REGISTER  [" + ue.label + "]",
               ue.label, Logger::CLR_ENB, "P-CSCF", Logger::CLR_PCSCF)
        .ie("From/To", ue.impu)
        .ie("Contact", "sip:ue@" + ue.ip + ":5060  ← 4G P-GW IP from EPC attach!")
        .ie("Expires", "3600  (re-REGISTER before expiry)")
        .ie("P-Access-Network-Info", "3GPP-E-UTRAN-FDD (tells IMS: UE is on 4G)")
        .ie("Authorization", "IMS-AKA Digest (different from EPS-AKA in EPC)")
        .state(ue.label, "UNREGISTERED → REGISTERING")
        .next("P-CSCF forwards to S-CSCF via I-CSCF (DNS NAPTR discovery)")
        .flush();

    std::string sip = SipText::buildRegister(ue.impu, ue.ip, ue.cseq++);
    pcapSip(sip, PcapWriter::IP_UE, PcapWriter::IP_PCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    VLog::step(2, 4, "SIP REGISTER  P-CSCF → S-CSCF",
               "P-CSCF", Logger::CLR_PCSCF, "S-CSCF", Logger::CLR_SCSCF)
        .ie("Route", "sip:scscf." + IMS_DOMAIN + ";lr")
        .ie("Via",   "P-CSCF added its own Via (route tracing)")
        .next("S-CSCF sends Cx SAR to HSS — registers as serving S-CSCF for this IMPU")
        .flush();
    pcapSip(sip, PcapWriter::IP_PCSCF, PcapWriter::IP_SCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    VLog::step(3, 4, "Diameter Cx SAR/SAA  [S-CSCF ↔ HSS]",
               "S-CSCF", Logger::CLR_SCSCF, "HSS", Logger::CLR_HSS)
        .ie("SAR", "Server-Assignment-Type: REGISTRATION")
        .ie("SAA", "iFC returned: trigger MTAS on REGISTER + INVITE")
        .ie("SAA", "MSISDN: +919000000001  APN: ims.mnc010.mcc404.3gppnetwork.org")
        .next("S-CSCF sends 3rd-party REGISTER to MTAS (ISC interface)")
        .flush();
    PcapWriter::instance().writeDiameter(DiameterCmd::SERVER_ASSIGNMENT, DiameterApp::CX, true,
        PcapWriter::IP_SCSCF, PcapWriter::PORT_DIA, PcapWriter::IP_HSS, PcapWriter::PORT_DIA);
    PcapWriter::instance().writeDiameter(DiameterCmd::SERVER_ASSIGNMENT, DiameterApp::CX, false,
        PcapWriter::IP_HSS, PcapWriter::PORT_DIA, PcapWriter::IP_SCSCF, PcapWriter::PORT_DIA);

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    VLog::step(4, 4, "SIP 200 OK  [Registration complete]",
               "S-CSCF", Logger::CLR_SCSCF, ue.label, Logger::CLR_ENB)
        .ie("P-Associated-URI", "tel:+919000000001  (MSISDN alias)")
        .ie("Service-Route",    "sip:scscf." + IMS_DOMAIN + ";lr")
        .ie("Expires",          "3600 — UE will re-REGISTER at ~1800s")
        .state(ue.label, "REGISTERING → REGISTERED")
        .next("MTAS notified via ISC — enables OIP/OIR, call waiting, forwarding for this UE")
        .flush();

    std::string ok200 = SipText::build200Register(ue.impu, ue.ip, ue.cseq);
    pcapSip(ok200, PcapWriter::IP_SCSCF, PcapWriter::IP_PCSCF);
    pcapSip(ok200, PcapWriter::IP_PCSCF, PcapWriter::IP_UE);

    ue.registered = true;
    Logger::sys("[" + ue.label + "] IMS registration complete ✓  IMPU=" + ue.impu);
}

// ── VoLTE call flow ───────────────────────────────────────────
static void doCall(const std::string& caller_id, const std::string& callee_id) {
    auto& caller = g_ues[caller_id];
    auto& callee = g_ues[callee_id];

    if (!caller.registered) { Logger::sys("[" + caller.label + "] not registered — run REG first"); return; }
    if (!callee.registered) { Logger::sys("[" + callee.label + "] not registered — run REG first"); return; }

    std::string call_id = "call-" + caller_id + callee_id + "-" + std::to_string(g_call_seq++);

    VLog::step(1, 8, "SIP INVITE  [VoLTE call setup]",
               caller.label, Logger::CLR_ENB, "P-CSCF", Logger::CLR_PCSCF)
        .ie("From",    caller.impu)
        .ie("To",      callee.impu)
        .ie("Call-ID", call_id + "  (unique per dialog — all messages share this)")
        .ie("SDP m=audio", std::to_string(50000) + " AMR-WB,AMR  (HD voice offer)")
        .ie("SDP m=video", std::to_string(50002) + " H264/90000")
        .ie("P-Preferred-Identity", caller.impu + "  (CLI to show callee)")
        .state(caller.label, "REGISTERED → CALLING")
        .next("P-CSCF validates caller identity, forwards to S-CSCF")
        .flush();

    std::string invite = SipText::buildInvite(caller.impu, callee.impu, caller.ip, call_id, caller.cseq);
    pcapSip(invite, PcapWriter::IP_UE, PcapWriter::IP_PCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    VLog::step(2, 8, "SIP INVITE  P-CSCF → S-CSCF",
               "P-CSCF", Logger::CLR_PCSCF, "S-CSCF", Logger::CLR_SCSCF)
        .ie("Route", "sip:scscf." + IMS_DOMAIN + ";lr")
        .next("S-CSCF checks iFC → triggers MTAS via ISC interface")
        .flush();
    pcapSip(invite, PcapWriter::IP_PCSCF, PcapWriter::IP_SCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    VLog::step(3, 8, "ISC INVITE  S-CSCF → MTAS  (Ericsson AS)",
               "S-CSCF", Logger::CLR_SCSCF, "MTAS", Logger::CLR_MTAS)
        .ie("Trigger", "iFC matched: method=INVITE → invoke MTAS")
        .ie("OIP",     "Originating Identity: +919000000001 — allowed to present CLI")
        .ie("Barring", "OIB check: called number not international — PASS")
        .ie("Service", "MMTEL: no forwarding active, no call barring active")
        .ie("CDR",     "Charging Data Record started for this call")
        .next("MTAS returns 'continue' → S-CSCF routes INVITE to callee S-CSCF/P-CSCF")
        .flush();
    // ISC is SIP — write as SIP on port 5060
    pcapSip(invite, PcapWriter::IP_SCSCF, PcapWriter::IP_MTAS);
    pcapSip(SipText::build100Trying(caller.impu, callee.impu, call_id, callee.cseq),
            PcapWriter::IP_MTAS, PcapWriter::IP_SCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    VLog::step(4, 8, "SIP 100 Trying",
               "S-CSCF", Logger::CLR_SCSCF, caller.label, Logger::CLR_ENB)
        .ie("Meaning", "Request received, processing  — stops retransmit timer")
        .next("S-CSCF routes INVITE toward callee (looks up callee in HSS via LIR/LIA)")
        .flush();
    pcapSip(SipText::build100Trying(caller.impu, callee.impu, call_id, callee.cseq),
            PcapWriter::IP_SCSCF, PcapWriter::IP_PCSCF);
    pcapSip(SipText::build100Trying(caller.impu, callee.impu, call_id, callee.cseq),
            PcapWriter::IP_PCSCF, PcapWriter::IP_UE);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    VLog::step(5, 8, "SIP 180 Ringing",
               callee.label, Logger::CLR_ENB, caller.label, Logger::CLR_ENB)
        .ie("To-tag",  "callee" + std::to_string(callee.cseq) + "  ← SIP DIALOG established here!")
        .ie("Meaning", "Callee's phone is ringing — caller hears ringback tone")
        .ie("PRACK",   "Reliable provisional response (RFC 3262) may be sent here")
        .state(callee.label, "REGISTERED → ALERTING")
        .next("Callee answers → 200 OK with SDP answer → media negotiation complete")
        .flush();
    std::string ring180 = SipText::build180Ringing(caller.impu, callee.impu, call_id, callee.cseq);
    pcapSip(ring180, 0x7F000002, PcapWriter::IP_PCSCF); // callee UE-B → P-CSCF
    pcapSip(ring180, PcapWriter::IP_PCSCF, PcapWriter::IP_SCSCF);
    pcapSip(ring180, PcapWriter::IP_SCSCF, PcapWriter::IP_UE);     // → caller

    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    VLog::step(6, 8, "SIP 200 OK  (callee answered)",
               callee.label, Logger::CLR_ENB, caller.label, Logger::CLR_ENB)
        .ie("SDP answer", "audio:60000/AMR-WB/16000  ← AMR-WB selected — HD Voice!")
        .ie("SDP answer", "video:60002/H264/90000")
        .ie("Codec",      "AMR-WB 12.65kbps — 16kHz sampling — voice sounds like in the room")
        .ie("RTP path",   caller.ip + ":50000 ←──RTP/UDP──→ " + callee.ip + ":60000")
        .state(callee.label, "ALERTING → IN-CALL")
        .next("Caller sends ACK → P-CSCF sends Diameter Rx AAR to PCRF → QCI=1 bearer!")
        .flush();
    std::string ok200inv = SipText::build200Invite(caller.impu, callee.impu, callee.ip, call_id, callee.cseq);
    pcapSip(ok200inv, 0x7F000002, PcapWriter::IP_SCSCF); // UE-B → S-CSCF
    pcapSip(ok200inv, PcapWriter::IP_SCSCF, PcapWriter::IP_PCSCF);
    pcapSip(ok200inv, PcapWriter::IP_PCSCF, PcapWriter::IP_UE);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    VLog::step(7, 8, "SIP ACK  +  Diameter Rx AAR",
               caller.label, Logger::CLR_ENB, "P-CSCF → PCRF", Logger::CLR_PCRF)
        .ie("ACK",      "Confirms 200 OK received — dialog fully established")
        .ie("Rx AAR",   "P-CSCF→PCRF: media=AMR-WB 12.65kbps, direction=sendrecv")
        .ie("AAR IE",   "Media-Component: codec=AMR-WB, bandwidth=12650bps")
        .ie("Result",   "PCRF→P-GW Gx RAR: install QCI=1 bearer rule")
        .ie("Bearer",   "eNB creates DRB QCI=1 — voice priority above data!")
        .state("Bearer", "DEFAULT(QCI=9) → + DEDICATED(QCI=1)")
        .next("RTP voice flows on QCI=1. Data (YouTube etc.) still on QCI=9.")
        .flush();
    pcapSip(SipText::buildAck(caller.impu, callee.impu, call_id, callee.cseq),
            PcapWriter::IP_UE, PcapWriter::IP_PCSCF);
    // Rx AAR to PCRF
    PcapWriter::instance().writeDiameter(DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, true,
        PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA, PcapWriter::IP_PCRF, PcapWriter::PORT_GX);
    PcapWriter::instance().writeDiameter(DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, false,
        PcapWriter::IP_PCRF, PcapWriter::PORT_GX, PcapWriter::IP_PCSCF, PcapWriter::PORT_DIA);

    VLog::step(8, 8, "CALL ACTIVE  — RTP flowing",
               caller.label, Logger::CLR_ENB, callee.label, Logger::CLR_ENB)
        .ie("Codec",   "AMR-WB/16000 — HD Voice")
        .ie("Bearer",  "QCI=1 dedicated bearer via eNB DRB")
        .ie("Path",    caller.ip + ":50000 ←──RTP/UDP:2152──► eNB ──► S-GW ──► P-GW ──► Internet")
        .ie("Latency", "< 100ms (QCI=1 guarantee)")
        .state(caller.label, "CALLING → IN-CALL")
        .next("Type BYE to end call — releases QCI=1 bearer via Rx STR to PCRF")
        .flush();

    caller.cseq++; callee.cseq++;
    Logger::sys("Call " + call_id + " ACTIVE ✓  [" + caller.label + "] ←→ [" + callee.label + "]");
}

// ── Conference flow ───────────────────────────────────────────
static void doConference() {
    Logger::sys("");
    VLog::step(1, 4, "CONFERENCE CALL SETUP  (A + B + C)",
               "MTAS", Logger::CLR_MTAS, "MRFC", Logger::CLR_SGW)
        .ie("Trigger",   "UE-A sends re-INVITE with conference URI")
        .ie("ISC",       "S-CSCF → MTAS: conference service invoked")
        .ie("Mr iface",  "MTAS → MRFC: SIP INVITE — create conference bridge")
        .ie("H.248",     "MRFC → MRFP: allocate 3-way audio mixing endpoint")
        .ie("Conf-URI",  "sip:conf-" + std::to_string(g_call_seq++) + "@mrfc." + IMS_DOMAIN)
        .next("MTAS INVITEs UE-B and UE-C to join conference URI")
        .flush();

    std::string conf_id = "conf-" + std::to_string(g_call_seq) + "@" + IMS_DOMAIN;
    std::string conf_invite_b = SipText::buildInvite(IMPU_A, IMPU_B, IP_UE_A, conf_id, 10, 50000);
    std::string conf_invite_c = SipText::buildInvite(IMPU_A, IMPU_C, IP_UE_A, conf_id, 11, 50000);
    pcapSip(conf_invite_b, PcapWriter::IP_SCSCF, 0x7F000002); // → UE-B
    pcapSip(conf_invite_c, PcapWriter::IP_SCSCF, 0x7F000003); // → UE-C

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    VLog::step(2, 4, "MRFC/MRFP — Conference bridge active",
               "MRFC", Logger::CLR_SGW, "MRFP", Logger::CLR_PGW)
        .ie("MRFC role",   "Controller: manages conference state, participant list")
        .ie("MRFP role",   "Processor: mixes 3 RTP streams, sends personalised mix to each")
        .ie("RTP mixing",  "UE-A hears UE-B+C. UE-B hears UE-A+C. UE-C hears UE-A+B")
        .ie("Protocol",    "MRFC↔MRFP: H.248/Megaco (port 2944) — not SIP!")
        .next("All three RTP streams → MRFP → mixed → QCI=1 bearer per UE")
        .flush();

    VLog::step(3, 4, "200 OK — All participants in conference",
               "MRFC", Logger::CLR_SGW, "UE-A/B/C", Logger::CLR_ENB)
        .ie("UE-A", IP_UE_A + ":50000 ──RTP──► MRFP ◄──RTP── " + IP_UE_B + ":60000")
        .ie("UE-C", IP_UE_C + ":70000 ──RTP──► MRFP (mixing all streams)")
        .ie("QCI-1", "3 × dedicated QCI=1 bearers — one per participant")
        .state("Call", "POINT-TO-POINT → CONFERENCE")
        .next("Any UE can leave: BYE removes one participant, MRFC adjusts mixing")
        .flush();

    VLog::step(4, 4, "INTERVIEW Q: MRFC vs MRFP vs MTAS",
               "MTAS", Logger::CLR_MTAS, "MRFC/MRFP", Logger::CLR_SGW)
        .ie("MTAS",  "Application Server — SERVICE LOGIC (when to conference, barring)")
        .ie("MRFC",  "Media Resource Function Controller — conference STATE machine")
        .ie("MRFP",  "Media Resource Function Processor — actual DSP mixing (audio/video)")
        .ie("ISC",   "S-CSCF ↔ MTAS: SIP (port 5060)")
        .ie("Mr",    "S-CSCF/MTAS ↔ MRFC: SIP (port 5060)")
        .ie("Cr",    "MRFC ↔ MRFP: H.248/Megaco (port 2944)")
        .flush();

    Logger::sys("Conference ACTIVE ✓  3 participants: UE-A, UE-B, UE-C");
}

// ── Call waiting ──────────────────────────────────────────────
static void doCallWait() {
    Logger::sys("");
    VLog::step(1, 3, "CALL WAITING  (UE-A in call, UE-C calls UE-A)",
               "UE-C", Logger::CLR_ENB, "MTAS", Logger::CLR_MTAS)
        .ie("Situation", "UE-A currently in active call with UE-B")
        .ie("UE-C",      "Sends SIP INVITE to UE-A")
        .ie("MTAS check","Dialog state: UE-A has active dialog — Call Waiting service applies")
        .next("MTAS sends re-INVITE to UE-A with Call-Waiting notification")
        .flush();

    std::string cw_id = "cw-" + std::to_string(g_call_seq++);
    pcapSip(SipText::buildInvite(IMPU_C, IMPU_A, IP_UE_C, cw_id, 5),
            PcapWriter::IP_UE + 2, PcapWriter::IP_PCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    VLog::step(2, 3, "MTAS applies Call Waiting service",
               "MTAS", Logger::CLR_MTAS, "UE-A", Logger::CLR_ENB)
        .ie("Action",  "MTAS sends 180 Ringing back to UE-C")
        .ie("Notify",  "UE-A hears audible beep: 2nd incoming call from UE-C")
        .ie("UE-A can","a) Accept: MTAS puts UE-B on HOLD (re-INVITE with a=inactive SDP)")
        .ie("",        "b) Reject: MTAS sends 486 Busy Here to UE-C")
        .ie("",        "c) Ignore: after 20s → forwarded to voicemail by MTAS")
        .next("UE-A accepts → MTAS holds UE-B, connects UE-C")
        .flush();

    VLog::step(3, 3, "UE-A accepts — UE-B put on HOLD",
               "MTAS", Logger::CLR_MTAS, "UE-B (HOLD)", Logger::CLR_SGW)
        .ie("HOLD",    "re-INVITE to UE-B: SDP a=sendonly (was a=sendrecv)")
        .ie("Meaning", "UE-B hears hold music — RTP from UE-B to MRFP still flows but mixed differently")
        .ie("Resume",  "UE-A ends UE-C call → re-INVITE to UE-B: a=sendrecv → resume")
        .state("UE-B", "IN-CALL → ON-HOLD")
        .state("UE-C", "CALLING → IN-CALL")
        .flush();

    Logger::sys("Call waiting scenario complete ✓");
}

// ── Call barring ──────────────────────────────────────────────
static void doBarring() {
    Logger::sys("");
    VLog::step(1, 2, "CALL BARRING  (OIB — Outgoing International)",
               "UE-A", Logger::CLR_ENB, "MTAS", Logger::CLR_MTAS)
        .ie("UE-A tries", "SIP INVITE To: sip:+44207XXXXXX@ims  (UK number)")
        .ie("MTAS checks","OIB (Outgoing International Barring) = ACTIVE for this subscriber")
        .ie("+44",        "UK country code = International = BARRED")
        .next("MTAS returns 603 Decline → S-CSCF → P-CSCF → UE-A")
        .flush();

    std::string barr_id = "barr-" + std::to_string(g_call_seq++);
    std::string intl_impu = "sip:+442071234567@ims.local";
    pcapSip(SipText::buildInvite(IMPU_A, intl_impu, IP_UE_A, barr_id, 20),
            PcapWriter::IP_UE, PcapWriter::IP_PCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    VLog::step(2, 2, "SIP 603 Decline  (OIB barring active)",
               "MTAS", Logger::CLR_MTAS, "UE-A", Logger::CLR_ENB)
        .ie("603",   "Decline — call barred by MTAS service logic")
        .ie("Reason","SIP;cause=603;text=\"OIB barring active\"")
        .ie("Types", "OIB=intl barring, BAIC=all incoming, BIC-Roam=incoming while roaming")
        .ie("UE-A",  "Displays: 'Call Barred' — no QCI=1 bearer created")
        .state("Call", "INVITE → BARRED (no call setup)")
        .flush();

    pcapSip(SipText::build603(IMPU_A, intl_impu, barr_id, 20, "OIB"),
            PcapWriter::IP_SCSCF, PcapWriter::IP_UE);
    Logger::sys("Call barring scenario complete ✓");
}

// ── BYE flow ──────────────────────────────────────────────────
static void doBye(const std::string& caller_id = "A", const std::string& callee_id = "B") {
    auto& caller = g_ues[caller_id];
    auto& callee = g_ues[callee_id];
    std::string call_id = "call-" + caller_id + callee_id + "-bye";

    VLog::step(1, 2, "SIP BYE  — ending call",
               caller.label, Logger::CLR_ENB, callee.label, Logger::CLR_ENB)
        .ie("Sent", "SIP BYE from " + caller.label)
        .ie("Flow", caller.label + " → P-CSCF → S-CSCF → MTAS → " + callee.label)
        .next("P-CSCF sends Diameter Rx STR to PCRF → QCI=1 bearer released")
        .flush();

    pcapSip(SipText::buildBye(caller.impu, callee.impu, call_id, caller.cseq),
            PcapWriter::IP_UE, PcapWriter::IP_PCSCF);
    pcapSip(SipText::buildBye(caller.impu, callee.impu, call_id, caller.cseq),
            PcapWriter::IP_PCSCF, PcapWriter::IP_SCSCF);

    std::this_thread::sleep_for(std::chrono::milliseconds(80));

    VLog::step(2, 2, "200 OK + Rx STR → QCI=1 bearer released",
               "P-CSCF", Logger::CLR_PCSCF, "PCRF", Logger::CLR_PCRF)
        .ie("200 OK",  "Call torn down — SIP dialog closed")
        .ie("Rx STR",  "P-CSCF → PCRF: Session-Termination-Request")
        .ie("Gx RAR",  "PCRF → P-GW: remove QCI=1 bearer rule")
        .ie("Result",  "QCI=1 bearer deleted — UE back to QCI=9 only")
        .state("Bearer", "QCI=1 DEDICATED → RELEASED")
        .flush();

    caller.cseq++; callee.cseq++;
    Logger::sys("Call ended ✓  QCI=1 bearer released");
}

// ── STATUS ────────────────────────────────────────────────────
static void doStatus() {
    Logger::sys("=== IMS Registration Status ===");
    for (auto& [id, ue] : g_ues) {
        Logger::sys("  " + ue.label + " : " +
                    (ue.registered ? "REGISTERED" : "not registered") +
                    "  IMPU=" + ue.impu + "  Contact=sip:ue@" + ue.ip + ":5060");
    }
}

// ── Startup diagram ───────────────────────────────────────────
static void printIMSStartup() {
    std::cout <<
        "\n"
        "  +============================================================+\n"
        "  |   IMS / VoLTE SIMULATOR — Three Terminals (A, B, C)       |\n"
        "  |   SIP visible in Wireshark — filter: sip || diameter       |\n"
        "  +============================================================+\n"
        "\n"
        "  PREREQUISITE: Run mme_sim first. UEs get 4G IPs from P-GW:\n"
        "    UE-A: 10.0.0.1  UE-B: 10.0.0.2  UE-C: 10.0.0.3\n"
        "  These IPs appear in the SIP Contact header below!\n"
        "\n"
        "  ARCHITECTURE:\n"
        "  [UE-A] [UE-B] [UE-C]\n"
        "     |      |      |\n"
        "     +------+------+--SIP:5060-->[P-CSCF]--SIP:5070-->[S-CSCF+MTAS]\n"
        "                                                              |\n"
        "                                                        Cx:3870\n"
        "                                                              |\n"
        "                                                         [IMS-HSS]\n"
        "\n"
        "  COMMANDS:\n"
        "    REG A|B|C|ALL  → IMS registration\n"
        "    CALL A B       → VoLTE call A→B\n"
        "    CALL A C       → VoLTE call A→C\n"
        "    CONF           → 3-party conference (A+B+C)\n"
        "    WAIT           → Call waiting scenario\n"
        "    BARR           → Call barring (OIB)\n"
        "    BYE            → End current call\n"
        "    STATUS         → Show registered UEs\n"
        "    QUIT\n"
        "  -------------------------------------------------------\n\n"
        << std::flush;
}

int main() {
    printIMSStartup();
    PcapWriter::instance().open("ims_capture.pcap");

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

    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    // Connect "UE" to P-CSCF
    Socket ue_conn;
    for (int i = 0; i < 30 && !stop.load(); ++i) {
        try { ue_conn = Socket::connectTo("127.0.0.1", 5060); break; }
        catch (...) { std::this_thread::sleep_for(std::chrono::milliseconds(200)); }
    }
    Logger::sys("[UE-simulator] connected to P-CSCF ✓");

    std::string line;
    std::cout << "\nims-sim> " << std::flush;

    while (!stop.load() && std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << "ims-sim> " << std::flush; continue; }

        std::istringstream iss(line);
        std::string cmd; iss >> cmd;
        for (auto& c : cmd) c = char(std::toupper(unsigned(c)));

        if (cmd == "QUIT") break;
        else if (cmd == "STATUS") doStatus();
        else if (cmd == "REG") {
            std::string who; iss >> who;
            for (auto& c : who) c = char(std::toupper(unsigned(c)));
            if (who == "ALL") { doRegister("A"); doRegister("B"); doRegister("C"); }
            else if (g_ues.count(who)) doRegister(who);
            else Logger::sys("Usage: REG A|B|C|ALL");
        }
        else if (cmd == "CALL") {
            std::string a, b; iss >> a >> b;
            for (auto& c : a) c = char(std::toupper(unsigned(c)));
            for (auto& c : b) c = char(std::toupper(unsigned(c)));
            if (g_ues.count(a) && g_ues.count(b)) doCall(a, b);
            else Logger::sys("Usage: CALL A B  or  CALL A C");
        }
        else if (cmd == "CONF") doConference();
        else if (cmd == "WAIT") doCallWait();
        else if (cmd == "BARR") doBarring();
        else if (cmd == "BYE")  doBye();
        else Logger::sys("Unknown. Try: REG ALL, CALL A B, CONF, WAIT, BARR, BYE, STATUS, QUIT");

        if (!stop.load()) std::cout << "\nims-sim> " << std::flush;
    }

    stop.store(true);
    hss_th.join(); scscf_th.join(); pcscf_th.join();
    PcapWriter::instance().close();
    Logger::sys("IMS simulator done. Open ims_capture.pcap in Wireshark — filter: sip");
    return 0;
}
