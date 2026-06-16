// json_event_log.h — writes sim_events.jsonl alongside pcap and text logs.
// Each line is one JSON object describing a protocol event; the Python
// visualizer (tools/viz_server.py) tails this file and enriches events with
// interview questions from tag_rules.json + interview_questions/*.yaml.
// Thread-safe via a single static mutex. Silently no-ops if file can't open.
#pragma once
#include <fstream>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <iomanip>
#include <algorithm>

namespace JsonEventLog {

struct MsgMeta {
    std::string beginner_text;
    std::string interview_q;
    std::string interview_a;
};

// Static lookup table: message-name substring → beginner/interview content
inline const std::unordered_map<std::string, MsgMeta>& getMsgTable() {
    static const std::unordered_map<std::string, MsgMeta> t {
        // ── 4G EPC ──────────────────────────────────────────────────────
        {"Diameter AIR", {
            "MME asks HSS: does this SIM card really belong to this subscriber?",
            "What is Diameter Authentication-Information-Request (AIR)?",
            "AIR (Command Code 318) is sent over S6a. MME sends IMSI and requests AUTN+RAND vectors from HSS. Ref: TS 29.272 §7.2.5."
        }},
        {"Diameter AIA", {
            "HSS replies with authentication vectors — the 'secret handshake' between the network and the SIM.",
            "What fields does AIA carry and why does it return multiple AVs?",
            "AIA carries E-UTRAN-Vector (RAND, XRES, AUTN, KASME). HSS returns 5 vectors for pre-fetching so MME can auth the UE without going to HSS again on each handover."
        }},
        {"Diameter ULR", {
            "MME tells HSS: this subscriber is now in my area, please send me their subscription profile.",
            "What triggers an Update-Location-Request (ULR)?",
            "ULR (CC 316) is sent after successful authentication. MME registers the new serving MME at HSS and requests the subscription data needed to authorize bearer establishment."
        }},
        {"Diameter ULA", {
            "HSS sends the subscriber's profile back — which services they can use, QoS, allowed APNs.",
            "What data is in the ULA Subscription-Data AVP?",
            "ULA carries MSISDN, AMBR, EPS-Subscribed-QoS-Profile, and APN-Configuration per bearer. The APN-AMBR limits per-APN throughput, separating it from UE-AMBR."
        }},
        {"GTPv2 Create Session Request", {
            "MME asks S-GW to open a data tunnel for the UE — like reserving a lane on the highway.",
            "What IEs are mandatory in a GTPv2-C Create Session Request?",
            "Mandatory IEs: IMSI (0x01), APN (0x47), RAT Type, F-TEID (MME/S-GW control plane), Bearer Contexts with EBI+QoS. Ref: TS 29.274 Table 7.2.1-1."
        }},
        {"GTPv2 Create Session Response", {
            "S-GW says 'tunnel ready' and gives the MME the data plane address and TEID for the eNB.",
            "How does GTPv2 Create Session Response differ between S-GW and P-GW responses?",
            "The S-GW CSResp includes the S1-U TEID for eNB. The P-GW CSResp adds the PDN Address Allocation (UE IP). Both responses are chained: P-GW→S-GW→MME."
        }},
        {"GTPv2 Modify Bearer", {
            "After handover, MME switches the data path to route traffic through the new eNB.",
            "When is Modify Bearer Request sent and what does it update?",
            "MBReq is sent after path switch (S1 HO) or during TAU. It updates the eNB TEID on S1-U so S-GW knows where to deliver DL packets. Ref: TS 29.274 §7.2.7."
        }},
        {"S1AP InitialContextSetupRequest", {
            "MME instructs the eNB to configure radio bearers and give the UE its 4G IP address.",
            "What security context does InitialContextSetupRequest carry?",
            "It carries the SecurityKey (KeNB, 256-bit), UESecurityCapabilities, and E-RAB list with S1-U TEID+IP for each bearer. eNB derives the AS keys (KRRCenc, KUPenc) from KeNB."
        }},
        {"NAS TAU Request", {
            "UE tells the network it has moved to a new Tracking Area (cell cluster) — like checking into a new city.",
            "Why does the UE send a TAU Request when entering a new TA?",
            "The UE sends TAU (NAS type 0x48) when it moves into a TA not in its Allowed TA List. MME validates the TAI, updates the UE's TA List, and resets T3412 (periodic TAU timer, default 54 min)."
        }},
        {"NAS TAU Accept", {
            "Network acknowledges the location update — now it knows where to page this UE.",
            "What timers are reset during TAU Accept?",
            "TAU Accept (NAS type 0x49) carries: new Allowed TA List, T3412 timer value (default 54 min = 0x23 01 in TLV), GUTI reallocation optional. T3412 expiry triggers next Periodic TAU."
        }},
        {"S1AP HandoverRequired", {
            "eNB signals the MME: this UE's signal is too weak here, please move it to a better tower.",
            "What triggers an S1 Handover vs an X2 Handover?",
            "S1 HO is used when source and target eNBs have no direct X2 link, or different MME pools. X2 HO bypasses the MME for the radio path but MME still does path switch. Ref: TS 36.413 §8.4."
        }},
        {"S1AP HandoverCommand", {
            "MME gives the target eNB's radio config to the source eNB to pass to the UE.",
            "What is in the HandoverCommand transparent container?",
            "The Transparent Container (TS 36.331 RRCConnectionReconfiguration) carries: target cell ID, DRB config, new security key (scg-Counter). The UE uses it to attach to the target eNB."
        }},
        {"S1AP HandoverNotify", {
            "New tower confirms the UE has successfully connected — handover complete from radio perspective.",
            "What happens between HandoverNotify and UEContextRelease in S1 HO?",
            "After HandoverNotify, MME sends Modify Bearer to S-GW to switch data path to target eNB TEID. Then MME sends UEContextRelease to source eNB to free its resources. Data forwarding stops."
        }},
        {"Diameter CCR", {
            "P-GW asks PCRF for QoS policy — the network's traffic cop for this connection.",
            "What is the role of Diameter CCR on the Gx interface?",
            "CCR-Initial (CC-Request-Type=1) is sent by PCEF (P-GW) to PCRF when a PDN session starts. PCRF replies with CCA carrying charging rules (QCI, MBR, GBR, TDF-Destination). Ref: TS 29.212."
        }},
        // ── 5G Core ──────────────────────────────────────────────────────
        {"HTTP NRF Register", {
            "Network node signs up with the 5G directory service — like a microservice registering with Consul.",
            "How does NRF registration differ from DNS-based service discovery in 4G?",
            "NRF uses HTTP/2 PUT (TS 29.510). Each NF registers its NF Profile (services, capacity, status). AMF queries NRF with GET /nnrf-disc/v1/nf-instances?target-nf-type=UDM. No static config needed."
        }},
        {"HTTP UDM Authentication", {
            "AMF asks UDM to generate the 5G-AKA authentication vectors for this subscriber.",
            "What is the 5G-AKA key hierarchy and how does it differ from EPS-AKA?",
            "5G-AKA uses SUPI (not IMSI), SUCI (ECIES-encrypted SUPI), and derives KAUSF→KSEAF→KAMF. The HXRES* prevents AMF replay attacks. Ref: TS 33.501 §6.1."
        }},
        {"HTTP AUSF Authentication", {
            "AUSF verifies the UE's response to the authentication challenge — the 5G equivalent of a secret handshake.",
            "What does AUSF validate in 5G-AKA?",
            "AUSF receives HXRES* from AMF, computes HXRES locally, compares them. If match, confirms authentication to AMF via Nausf_UEAuthentication_Authenticate response. Prevents AMF lying about UE."
        }},
        {"HTTP SMF PDUSession", {
            "AMF asks SMF to set up a data session — allocating an IP address and a tunnel to the internet.",
            "What HTTP method and URL does AMF use to create a PDU session at SMF?",
            "POST /nsmf-pdusession/v1/sm-contexts with body containing SUPI, DNN, NSSAI, and PDU type. SMF responds 201 Created with smContextRef and UE IP. Ref: TS 29.502 §5.2.2."
        }},
        {"PFCP Session Establishment Request", {
            "SMF instructs the UPF (the data plane): here are the rules for forwarding this UE's traffic.",
            "What is the difference between a PDR and a FAR in PFCP?",
            "PDR (Packet Detection Rule) identifies packets by UE IP, TEID, or SDF filter. FAR (Forwarding Action Rule) says what to do: FORWARD to N6/N3, DROP, or BUFFER. SMF pushes PDR+FAR pairs to UPF via N4."
        }},
        {"PFCP Session Establishment Response", {
            "UPF confirms it has installed the forwarding rules — data plane is now ready.",
            "What is a SEID in PFCP and why is it needed?",
            "SEID (Session Endpoint Identifier) is a 64-bit handle for the PFCP session. SMF assigns the UPF SEID; UPF assigns the SMF SEID. All subsequent PFCP messages use both SEIDs to identify the session."
        }},
        // ── IMS / VoLTE ──────────────────────────────────────────────────
        {"SIP REGISTER", {
            "VoLTE phone registers with the IMS network — like logging into a VoIP account.",
            "Why does SIP REGISTER go to P-CSCF first and not directly to S-CSCF?",
            "P-CSCF is discovered via PCO/DNS at PDN attach time. It enforces security (IPSec), compresses SIP headers (RFC 5049), and routes to the correct S-CSCF via I-CSCF. Direct S-CSCF access would bypass these."
        }},
        {"SIP 401 Unauthorized", {
            "IMS network challenges the phone: prove who you are by responding to this cryptographic challenge.",
            "How does SIP Digest authentication work in IMS and what replaces it in VoLTE?",
            "RFC 3261 Digest uses MD5 nonce challenge. In IMS/VoLTE (TS 24.229), IMS-AKA replaces Digest: S-CSCF fetches MAV from HSS via Diameter Cx, embeds RAND+AUTN in WWW-Authenticate header."
        }},
        {"SIP INVITE", {
            "Caller's phone sends the call setup request — like dialing a number in VoIP.",
            "What SDP fields are mandatory in a VoLTE SIP INVITE per IR.92?",
            "IR.92 mandates: AMR-WB codec (RTP/AVP 116), RTCP-MUX, telephone-event (DTMF), ICE-lite, and precondition headers (RFC 4032). Missing AMR-WB results in 488 Not Acceptable Here."
        }},
        {"SIP 183 Session Progress", {
            "Callee's network says 'ringing' — the early media path is set up before the call is answered.",
            "What is the difference between SIP 180 Ringing and 183 Session Progress in VoLTE?",
            "180 has no SDP (no media yet). 183 carries early SDP enabling the caller to hear ringback tone from the callee's side (early media, RFC 3959). 183 triggers PRACK (RFC 3262) for reliable delivery."
        }},
        {"SIP 200 OK", {
            "Call connected — both phones agree on codec and media port.",
            "Why is ACK sent as a separate SIP transaction for INVITE?",
            "INVITE uses a 3-way handshake (INVITE/200/ACK) because the 200 OK carries SDP and needs reliable delivery. ACK is not retransmitted — if lost, callee retransmits 200 OK. Other methods use 2-way only."
        }},
        {"SIP BYE", {
            "One party hangs up — this message tears down the call and releases network resources.",
            "What network resources must be released after SIP BYE in VoLTE?",
            "P-CSCF sends Diameter STR/AAR to PCRF to release the dedicated GBR bearer (QCI=1). S-CSCF sends Diameter SAR to HSS to update registration state. P-GW deletes the dedicated bearer via GTPv2 Delete Bearer."
        }},
        {"Diameter MAR", {
            "S-CSCF asks the IMS-HSS to authenticate the SIP REGISTER — the IMS equivalent of EPS-AKA.",
            "What Diameter interface does S-CSCF use to talk to HSS and what does MAR carry?",
            "Cx interface (TS 29.228). MAR (Multimedia-Auth-Request, CC 303) carries IMPU, IMPI, and SIP-Auth-Data-Item requesting IMS-AKA vectors (RAND, AUTN, XRES, CK, IK)."
        }},
        {"Diameter SAR", {
            "After registration, S-CSCF tells HSS it is now serving this subscriber — so HSS routes future calls here.",
            "What does the Server-Assignment-Request tell the HSS and why is it needed?",
            "SAR (CC 301) carries IMPU, IMPI, and Server-Assignment-Type (REGISTRATION). HSS stores the S-CSCF name, downloads the service profile (iFC) to S-CSCF, and will route terminating requests to this S-CSCF."
        }},
    };
    return t;
}

inline std::mutex& getFileMutex() { static std::mutex m; return m; }

inline std::ofstream& getFile() {
    static std::ofstream f;
    if (!f.is_open()) {
        f.open("sim_events.jsonl", std::ios::out | std::ios::app);
    }
    return f;
}

// JSON-escape: replace " with \" and \ with \\, strip control chars
inline std::string jsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"')  { out += "\\\""; }
        else if (c == '\\') { out += "\\\\"; }
        else if (c == '\n') { out += "\\n"; }
        else if (c == '\r') { out += "\\r"; }
        else if (c == '\t') { out += "\\t"; }
        else if (static_cast<uint8_t>(c) < 0x20) {}  // drop other controls
        else { out += c; }
    }
    return out;
}

inline std::string toHex(const std::vector<uint8_t>& bytes) {
    if (bytes.empty()) return "";
    std::ostringstream ss;
    for (auto b : bytes) ss << std::hex << std::setw(2) << std::setfill('0') << (int)b;
    return ss.str();
}

// Look up beginner/interview content by searching for msg as a substring key
inline const MsgMeta* lookup(const std::string& msg) {
    const auto& t = getMsgTable();
    // exact match first
    auto it = t.find(msg);
    if (it != t.end()) return &it->second;
    // substring match
    for (const auto& kv : t) {
        if (msg.find(kv.first) != std::string::npos ||
            kv.first.find(msg) != std::string::npos) {
            return &kv.second;
        }
    }
    return nullptr;
}

// Core API: log one protocol event
inline void logEvent(
    const std::string& from,
    const std::string& to,
    const std::string& msg,
    const std::string& interface_name,
    int port,
    const std::vector<uint8_t>& hex_bytes,
    const std::string& sim  // "4g" | "5g" | "ims"
) {
    auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    const MsgMeta* meta = lookup(msg);
    std::string beginner = meta ? meta->beginner_text : "";
    std::string q        = meta ? meta->interview_q   : "";
    std::string a        = meta ? meta->interview_a   : "";

    std::string hex = toHex(hex_bytes);

    std::ostringstream line;
    line << "{"
         << "\"ts\":"    << now_ms << ","
         << "\"from\":\"" << jsonEsc(from)           << "\","
         << "\"to\":\""   << jsonEsc(to)             << "\","
         << "\"msg\":\""  << jsonEsc(msg)            << "\","
         << "\"interface\":\"" << jsonEsc(interface_name) << "\","
         << "\"port\":"   << port                    << ","
         << "\"hex\":\""  << hex                     << "\","
         << "\"sim\":\""  << jsonEsc(sim)            << "\","
         << "\"beginner_text\":\"" << jsonEsc(beginner) << "\","
         << "\"interview_q\":\""   << jsonEsc(q)     << "\","
         << "\"interview_a\":\""   << jsonEsc(a)     << "\""
         << "}";

    std::lock_guard<std::mutex> lk(getFileMutex());
    auto& f = getFile();
    if (f.is_open()) {
        f << line.str() << "\n";
        f.flush();
    }
}

// Convenience overload with no hex bytes
inline void logEvent(
    const std::string& from,
    const std::string& to,
    const std::string& msg,
    const std::string& interface_name,
    int port,
    const std::string& sim
) {
    logEvent(from, to, msg, interface_name, port, {}, sim);
}

} // namespace JsonEventLog
