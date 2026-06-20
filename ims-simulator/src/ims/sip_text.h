#pragma once
// ============================================================
// SIP TEXT BUILDER — generates real RFC 3261 SIP messages
//
// Why text format?
//   SIP is a text-based protocol (like HTTP).
//   If we write these bytes to TCP port 5060, Wireshark
//   automatically decodes every header field — From, To,
//   Call-ID, SDP, Via — and shows "SIP" in Protocol column.
//
// INTERVIEW: "What is SIP?"
//   Session Initiation Protocol (RFC 3261). Text-based (UTF-8),
//   modelled on HTTP. Establishes, modifies, terminates
//   multimedia sessions. DOES NOT carry voice — that's RTP.
//   SIP = signalling only. Voice goes on UDP via RTP.
//
// KEY INTERVIEW IEs (headers):
//   From/To        : Dialog participant identities (IMPU)
//   Call-ID        : Unique per dialog — ties all messages in one call
//   CSeq           : Sequence number per method — detects out-of-order
//   Via            : Route tracing — each proxy adds its own Via
//   Contact        : UE's actual IP:port — where to send future requests
//   Expires        : Registration lifetime in seconds
//   P-Preferred-Identity : CLI — number to show the called party
//   P-Access-Network-Info: Tells IMS the UE is on 4G (3GPP-E-UTRAN)
//   SDP m=audio    : RTP port + codec list for voice
//   SDP a=rtpmap   : Codec details (AMR-WB = HD voice at 16kHz)
// ============================================================
#include <string>
#include <sstream>

// IMS domain and user identities
static const std::string IMS_DOMAIN = "ims.mnc010.mcc404.3gppnetwork.org";
static const std::string IMPU_A = "sip:+919000000001@" + IMS_DOMAIN;
static const std::string IMPU_B = "sip:+919000000002@" + IMS_DOMAIN;
static const std::string IMPU_C = "sip:+919000000003@" + IMS_DOMAIN;
static const std::string IP_UE_A = "10.0.0.1"; // from 4G attach!
static const std::string IP_UE_B = "10.0.0.2";
static const std::string IP_UE_C = "10.0.0.3";

namespace SipText {

// ── SIP REGISTER ─────────────────────────────────────────────
// UE → P-CSCF: "I am reachable at this IP for incoming calls"
//
// KEY IEs for interview:
//   Contact: UE's 4G IP (from P-GW allocation in EPC attach!)
//   P-Access-Network-Info: 3GPP-E-UTRAN — tells IMS it's 4G
//   Expires: 3600 — UE must re-REGISTER before this expires
//   Authorization: IMS-AKA digest (different from EPS-AKA in EPC)
inline std::string buildRegister(const std::string& impu,
                                  const std::string& ue_ip,
                                  int cseq, int expires = 3600) {
    std::ostringstream ss;
    ss << "REGISTER sip:" << IMS_DOMAIN << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP " << ue_ip << ":5060;branch=z9hG4bKreg" << cseq << "\r\n"
       << "Max-Forwards: 70\r\n"
       << "From: " << impu << ";tag=reg" << cseq << "\r\n"
       << "To: " << impu << "\r\n"
       << "Call-ID: reg-" << cseq << "@" << ue_ip << "\r\n"
       << "CSeq: " << cseq << " REGISTER\r\n"
       << "Contact: <sip:ue@" << ue_ip << ":5060>;expires=" << expires << "\r\n"
       << "Expires: " << expires << "\r\n"
       << "P-Access-Network-Info: 3GPP-E-UTRAN-FDD; utran-cell-id-3gpp=404010001\r\n"
       << "Authorization: Digest username=\"" << impu << "\","
       << " realm=\"" << IMS_DOMAIN << "\", nonce=\"\", uri=\"sip:" << IMS_DOMAIN << "\"\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP 200 OK (REGISTER) ─────────────────────────────────────
inline std::string build200Register(const std::string& impu,
                                     const std::string& ue_ip, int cseq) {
    std::ostringstream ss;
    ss << "SIP/2.0 200 OK\r\n"
       << "Via: SIP/2.0/TCP " << ue_ip << ":5060;branch=z9hG4bKreg" << cseq << "\r\n"
       << "From: " << impu << ";tag=reg" << cseq << "\r\n"
       << "To: " << impu << ";tag=scscf001\r\n"
       << "Call-ID: reg-" << cseq << "@" << ue_ip << "\r\n"
       << "CSeq: " << cseq << " REGISTER\r\n"
       << "Contact: <sip:ue@" << ue_ip << ":5060>;expires=3600\r\n"
       << "P-Associated-URI: <tel:+919000000001>\r\n"
       << "Service-Route: <sip:scscf." << IMS_DOMAIN << ";lr>\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP INVITE ────────────────────────────────────────────────
// KEY IEs for interview:
//   P-Preferred-Identity: CLI the caller wants to show callee
//   SDP m=audio: RTP port + codec list (offer)
//   SDP a=rtpmap: AMR-WB/16000 = HD Voice (16kHz)
//   SDP a=sendrecv: bidirectional media
//   Content-Type + Content-Length: required for SDP body
inline std::string buildInvite(const std::string& from_impu,
                                 const std::string& to_impu,
                                 const std::string& from_ip,
                                 const std::string& call_id,
                                 int cseq, int rtp_port = 50000) {
    std::string sdp =
        "v=0\r\n"
        "o=ue 12345 67890 IN IP4 " + from_ip + "\r\n"
        "s=VoLTE Call\r\n"
        "c=IN IP4 " + from_ip + "\r\n"
        "t=0 0\r\n"
        "m=audio " + std::to_string(rtp_port) + " RTP/AVP 98 99\r\n"
        "a=rtpmap:98 AMR-WB/16000\r\n"   // HD Voice codec
        "a=rtpmap:99 AMR/8000\r\n"        // fallback
        "a=sendrecv\r\n"
        "m=video " + std::to_string(rtp_port+2) + " RTP/AVP 100\r\n"
        "a=rtpmap:100 H264/90000\r\n"
        "a=sendrecv\r\n";

    std::ostringstream ss;
    ss << "INVITE " << to_impu << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP " << from_ip << ":5060;branch=z9hG4bKinv" << cseq << "\r\n"
       << "Max-Forwards: 70\r\n"
       << "From: " << from_impu << ";tag=inv" << cseq << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " INVITE\r\n"
       << "Contact: <sip:ue@" << from_ip << ":5060>\r\n"
       << "P-Preferred-Identity: " << from_impu << "\r\n"
       << "P-Access-Network-Info: 3GPP-E-UTRAN-FDD; utran-cell-id-3gpp=404010001\r\n"
       << "Supported: 100rel,precondition\r\n"
       << "Content-Type: application/sdp\r\n"
       << "Content-Length: " << sdp.size() << "\r\n\r\n"
       << sdp;
    return ss.str();
}

// ── SIP 100 Trying ────────────────────────────────────────────
inline std::string build100Trying(const std::string& from_impu,
                                    const std::string& to_impu,
                                    const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "SIP/2.0 100 Trying\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " INVITE\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP 183 Session Progress ──────────────────────────────────
// KEY for VoLTE interviews:
//   Sent BEFORE 180 Ringing — carries SDP answer for early media
//   QoS preconditions (RFC 3312): network must reserve QCI=1 bearer
//   BEFORE alerting the callee. Shows a=curr/a=des lines.
//   RSeq header: reliability sequence (requires PRACK to confirm)
//   Require: 100rel — makes this provisional reliable (VoLTE mandatory)
//   Flow: INVITE → 100 Trying → 183 Session Progress → PRACK
//         → 200 OK PRACK → 180 Ringing → 200 OK → ACK
inline std::string build183SessionProgress(const std::string& from_impu,
                                            const std::string& to_impu,
                                            const std::string& call_id,
                                            int cseq,
                                            int callee_rtp_port = 60000) {
    std::string sdp =
        "v=0\r\n"
        "o=ue-b 11111 22222 IN IP4 10.0.0.2\r\n"
        "s=VoLTE Call\r\n"
        "c=IN IP4 10.0.0.2\r\n"
        "t=0 0\r\n"
        "m=audio " + std::to_string(callee_rtp_port) + " RTP/AVP 98\r\n"
        "a=rtpmap:98 AMR-WB/16000\r\n"
        "a=curr:qos local none\r\n"        // QoS not yet reserved locally
        "a=curr:qos remote none\r\n"       // QoS not yet reserved remotely
        "a=des:qos mandatory local sendrecv\r\n"  // MUST reserve before alerting
        "a=des:qos mandatory remote sendrecv\r\n"
        "a=sendrecv\r\n";

    std::ostringstream ss;
    ss << "SIP/2.0 183 Session Progress\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << ";tag=early" << cseq << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " INVITE\r\n"
       << "RSeq: 1\r\n"
       << "Require: 100rel\r\n"
       << "Contact: <sip:ue-b@10.0.0.2:5060>\r\n"
       << "Content-Type: application/sdp\r\n"
       << "Content-Length: " << sdp.size() << "\r\n\r\n"
       << sdp;
    return ss.str();
}

// ── SIP 180 Ringing ───────────────────────────────────────────
// KEY IE: To-tag — this is when the SIP dialog is established!
inline std::string build180Ringing(const std::string& from_impu,
                                     const std::string& to_impu,
                                     const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "SIP/2.0 180 Ringing\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << ";tag=callee" << cseq << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " INVITE\r\n"
       << "Contact: <sip:ue-b@10.0.0.2:5060>\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP 200 OK (INVITE) ───────────────────────────────────────
// KEY IEs: SDP answer (codec selected), To-tag, Contact
// SDP answer: callee picks ONE codec from offer (AMR-WB selected)
inline std::string build200Invite(const std::string& from_impu,
                                    const std::string& to_impu,
                                    const std::string& callee_ip,
                                    const std::string& call_id,
                                    int cseq, int callee_rtp_port = 60000) {
    std::string sdp =
        "v=0\r\n"
        "o=ue-b 11111 22222 IN IP4 " + callee_ip + "\r\n"
        "s=VoLTE Call\r\n"
        "c=IN IP4 " + callee_ip + "\r\n"
        "t=0 0\r\n"
        "m=audio " + std::to_string(callee_rtp_port) + " RTP/AVP 98\r\n"
        "a=rtpmap:98 AMR-WB/16000\r\n"    // AMR-WB selected — HD Voice!
        "a=sendrecv\r\n"
        "m=video " + std::to_string(callee_rtp_port+2) + " RTP/AVP 100\r\n"
        "a=rtpmap:100 H264/90000\r\n"
        "a=sendrecv\r\n";

    std::ostringstream ss;
    ss << "SIP/2.0 200 OK\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << ";tag=callee" << cseq << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " INVITE\r\n"
       << "Contact: <sip:ue-b@" << callee_ip << ":5060>\r\n"
       << "Content-Type: application/sdp\r\n"
       << "Content-Length: " << sdp.size() << "\r\n\r\n"
       << sdp;
    return ss.str();
}

// ── SIP ACK ───────────────────────────────────────────────────
inline std::string buildAck(const std::string& from_impu,
                              const std::string& to_impu,
                              const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "ACK " << to_impu << " SIP/2.0\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << ";tag=callee" << cseq << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " ACK\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP BYE ───────────────────────────────────────────────────
inline std::string buildBye(const std::string& from_impu,
                              const std::string& to_impu,
                              const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "BYE " << to_impu << " SIP/2.0\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " BYE\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP 200 OK (BYE) ──────────────────────────────────────────
inline std::string build200Bye(const std::string& from_impu,
                                 const std::string& to_impu,
                                 const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "SIP/2.0 200 OK\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " BYE\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP PRACK (RFC 3262) ───────────────────────────────────────
// Acknowledges reliable provisional response (180 Ringing with 100rel)
// RAck header: RSeq from 180 + CSeq from INVITE + method
// VoLTE mandates Require:100rel (TS 24.229 §5.1.1.1)
inline std::string buildPrack(const std::string& from_impu,
                                const std::string& to_impu,
                                const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "PRACK " << to_impu << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP 10.0.0.8:5060;branch=z9hG4bKpr" << cseq << "\r\n"
       << "Max-Forwards: 70\r\n"
       << "From: " << from_impu << ";tag=inv" << cseq << "\r\n"
       << "To: " << to_impu << ";tag=callee" << cseq << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " PRACK\r\n"
       << "RAck: 1 1 INVITE\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP 200 OK (PRACK) ────────────────────────────────────────
inline std::string build200Prack(const std::string& from_impu,
                                   const std::string& to_impu,
                                   const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "SIP/2.0 200 OK\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " PRACK\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP re-INVITE ─────────────────────────────────────────────
// Generic re-INVITE builder for HOLD/RESUME/VIDEO/VOICE.
// Caller supplies the SDP body directly (controls a=sendonly/sendrecv,
// m=video port=0, etc.) so each scenario shows the relevant SDP diff.
inline std::string buildReInvite(const std::string& from_impu,
                                  const std::string& to_impu,
                                  const std::string& from_ip,
                                  const std::string& call_id,
                                  int cseq,
                                  const std::string& sdp_body) {
    std::ostringstream ss;
    ss << "INVITE " << to_impu << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP " << from_ip << ":5060;branch=z9hG4bKre" << cseq << "\r\n"
       << "Max-Forwards: 70\r\n"
       << "From: " << from_impu << ";tag=re" << cseq << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " INVITE\r\n"
       << "Content-Type: application/sdp\r\n"
       << "Content-Length: " << sdp_body.size() << "\r\n\r\n"
       << sdp_body;
    return ss.str();
}

// ── SIP REFER (RFC 3515) ──────────────────────────────────────
// UE-A asks S-CSCF to add UE-C to the call.
// KEY IEs:
//   Refer-To: sip:UE-C — who to invite to the conference
//   Referred-By: UE-A — who is initiating the transfer
//   Refer-Sub: false — don't create implicit subscription (if present)
// S-CSCF responds 202 Accepted (NOT 200 OK — REFER is async)
// S-CSCF then sends INVITE to UE-C on UE-A's behalf
// S-CSCF sends NOTIFY to UE-A tracking progress of the REFER
inline std::string buildRefer(const std::string& from_impu,
                                const std::string& to_impu,
                                const std::string& refer_to_impu,
                                const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "REFER " << to_impu << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP 10.0.0.8:5060;branch=z9hG4bKref" << cseq << "\r\n"
       << "Max-Forwards: 70\r\n"
       << "From: " << from_impu << ";tag=inv" << cseq << "\r\n"
       << "To: " << to_impu << ";tag=callee" << cseq << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " REFER\r\n"
       << "Refer-To: <" << refer_to_impu << ">\r\n"
       << "Referred-By: <" << from_impu << ">\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP 202 Accepted (response to REFER) ──────────────────────
// NOT 200 OK — REFER response is 202 because REFER is async.
// Means: "I received your REFER request, processing it."
// NOTIFY messages will follow to report progress.
inline std::string build202Accepted(const std::string& from_impu,
                                     const std::string& to_impu,
                                     const std::string& call_id, int cseq) {
    std::ostringstream ss;
    ss << "SIP/2.0 202 Accepted\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " REFER\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP NOTIFY (REFER status — RFC 3515) ──────────────────────
// S-CSCF → UE-A: progress updates on the REFER
// Body: message/sipfrag carrying the SIP response code
// States: "trying" → "early" (UE-C ringing) → "terminated" (done)
inline std::string buildNotifyRefer(const std::string& from_impu,
                                     const std::string& to_impu,
                                     const std::string& call_id, int cseq,
                                     const std::string& sub_state,   // active, terminated
                                     const std::string& sip_frag) {  // "SIP/2.0 100 Trying"
    std::string body = sip_frag + "\r\n";
    std::ostringstream ss;
    ss << "NOTIFY " << from_impu << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP 10.0.0.9:5070;branch=z9hG4bKnot" << cseq << "\r\n"
       << "From: " << to_impu << ";tag=scscf\r\n"
       << "To: " << from_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " NOTIFY\r\n"
       << "Event: refer\r\n"
       << "Subscription-State: " << sub_state << "\r\n"
       << "Content-Type: message/sipfrag;version=2.0\r\n"
       << "Content-Length: " << body.size() << "\r\n\r\n"
       << body;
    return ss.str();
}

// ── SIP SUBSCRIBE (RFC 4575 — Conference State) ───────────────
// UE-A subscribes to conference-state package at the MRFC.
// MRFC sends NOTIFY with XML body listing all participants.
// Event: conference — triggers conference-info XML NOTIFYs
// When participant joins/leaves: new NOTIFY is pushed automatically
inline std::string buildSubscribe(const std::string& from_impu,
                                    const std::string& conf_uri,
                                    const std::string& call_id, int cseq,
                                    int expires = 3600) {
    std::ostringstream ss;
    ss << "SUBSCRIBE " << conf_uri << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP 10.0.0.1:5060;branch=z9hG4bKsub" << cseq << "\r\n"
       << "From: " << from_impu << ";tag=sub" << cseq << "\r\n"
       << "To: " << conf_uri << "\r\n"
       << "Call-ID: " << call_id << "-sub\r\n"
       << "CSeq: " << cseq << " SUBSCRIBE\r\n"
       << "Event: conference\r\n"
       << "Accept: application/conference-info+xml\r\n"
       << "Expires: " << expires << "\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

// ── SIP NOTIFY (Conference State — RFC 4575) ──────────────────
// MRFC → UE-A: XML listing all conference participants.
// Body: conference-info+xml (who is connected, their status, media)
// KEY for interview: this is how the "conference widget" on your phone
// knows how many people are in the call and who they are.
inline std::string buildNotifyConf(const std::string& conf_uri,
                                     const std::string& to_impu,
                                     const std::string& call_id,
                                     int cseq,
                                     const std::string& p_a,
                                     const std::string& p_b,
                                     const std::string& p_c) {
    std::string xml =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\r\n"
        "<conference-info xmlns=\"urn:ietf:params:xml:ns:conference-info\"\r\n"
        "  entity=\"" + conf_uri + "\" state=\"full\" version=\"1\">\r\n"
        "  <conference-description><display-text>3-Party Conference</display-text></conference-description>\r\n"
        "  <users>\r\n"
        "    <user entity=\"" + p_a + "\" state=\"full\">\r\n"
        "      <display-text>UE-A</display-text>\r\n"
        "      <endpoint entity=\"sip:ue@10.0.0.1:5060\">\r\n"
        "        <status>connected</status>\r\n"
        "        <media id=\"1\"><type>audio</type><status>recvonly</status></media>\r\n"
        "      </endpoint>\r\n"
        "    </user>\r\n"
        "    <user entity=\"" + p_b + "\" state=\"full\">\r\n"
        "      <display-text>UE-B</display-text>\r\n"
        "      <endpoint entity=\"sip:ue@10.0.0.2:5060\">\r\n"
        "        <status>connected</status>\r\n"
        "        <media id=\"1\"><type>audio</type><status>recvonly</status></media>\r\n"
        "      </endpoint>\r\n"
        "    </user>\r\n"
        "    <user entity=\"" + p_c + "\" state=\"full\">\r\n"
        "      <display-text>UE-C</display-text>\r\n"
        "      <endpoint entity=\"sip:ue@10.0.0.3:5060\">\r\n"
        "        <status>connected</status>\r\n"
        "        <media id=\"1\"><type>audio</type><status>recvonly</status></media>\r\n"
        "      </endpoint>\r\n"
        "    </user>\r\n"
        "  </users>\r\n"
        "</conference-info>\r\n";

    std::ostringstream ss;
    ss << "NOTIFY " << to_impu << " SIP/2.0\r\n"
       << "Via: SIP/2.0/TCP 10.0.0.11:5060;branch=z9hG4bKncf" << cseq << "\r\n"
       << "From: " << conf_uri << ";tag=mrfc\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "-sub\r\n"
       << "CSeq: " << cseq << " NOTIFY\r\n"
       << "Event: conference\r\n"
       << "Subscription-State: active;expires=3600\r\n"
       << "Content-Type: application/conference-info+xml\r\n"
       << "Content-Length: " << xml.size() << "\r\n\r\n"
       << xml;
    return ss.str();
}

// ── SIP 603 Decline (call barring) ────────────────────────────
inline std::string build603(const std::string& from_impu,
                              const std::string& to_impu,
                              const std::string& call_id, int cseq,
                              const std::string& reason = "OIB") {
    std::ostringstream ss;
    ss << "SIP/2.0 603 Decline\r\n"
       << "From: " << from_impu << "\r\n"
       << "To: " << to_impu << "\r\n"
       << "Call-ID: " << call_id << "\r\n"
       << "CSeq: " << cseq << " INVITE\r\n"
       << "Reason: SIP;cause=603;text=\"" << reason << " barring active\"\r\n"
       << "Content-Length: 0\r\n\r\n";
    return ss.str();
}

} // namespace SipText
