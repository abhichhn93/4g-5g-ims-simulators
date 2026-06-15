#pragma once
#include <cstdint>
#include <string>
#include "common/message_types.h"
#include "common/tlv.h"

// ============================================================
// SIP MESSAGE TYPES — used in IMS simulator
//
// REAL SIP (RFC 3261):
//   Text-based protocol over UDP/TCP/TLS
//   Port 5060 (UDP/TCP), 5061 (TLS)
//   Methods: REGISTER, INVITE, ACK, BYE, CANCEL, OPTIONS, UPDATE, PRACK
//   Responses: 1xx=Provisional, 2xx=Success, 3xx=Redirect, 4xx=Client Error
//
// OUR SIM: binary TLV over TCP (same pattern as S1AP/Diameter)
//   Easier to parse, still shows the correct call flow and IEs
//
// MTAS ROLE: Ericsson's MTAS is an Application Server (AS) in IMS.
//   S-CSCF invokes MTAS via Third Party Registration and service triggers.
//   MTAS handles: VoLTE codec negotiation, supplementary services
//   (call waiting, call forwarding, conference, CLIP/CLIR/OIP/OIR)
// ============================================================

// ── IMS / SIP message types ──────────────────────────────────
enum class SipMsgType : uint16_t {
    // SIP Requests
    SIP_REGISTER    = 0x0501,  // UE → P-CSCF: register with IMS
    SIP_INVITE      = 0x0502,  // Caller → P-CSCF: initiate call
    SIP_ACK         = 0x0503,  // Caller → Callee: confirm 200 OK
    SIP_BYE         = 0x0504,  // Either → other: terminate call
    SIP_CANCEL      = 0x0505,  // Caller: cancel pending INVITE

    // SIP Responses
    SIP_100_TRYING      = 0x0510,  // Provisional — request received
    SIP_180_RINGING     = 0x0511,  // Provisional — UE-B alerting
    SIP_183_PROGRESS    = 0x0512,  // Provisional — session in progress (early media)
    SIP_200_OK          = 0x0513,  // Success

    // Diameter Cx interface (S-CSCF ↔ HSS) — TS 29.229
    DIA_CX_UAR = 0x0601,  // User-Authorization-Request: I-CSCF asks HSS for S-CSCF
    DIA_CX_UAA = 0x0602,  // User-Authorization-Answer: HSS returns S-CSCF name
    DIA_CX_SAR = 0x0603,  // Server-Assignment-Request: S-CSCF registers in HSS
    DIA_CX_SAA = 0x0604,  // Server-Assignment-Answer: HSS sends subscriber profile

    // Diameter Rx interface (P-CSCF ↔ PCRF) — TS 29.214
    // Triggered when media negotiation is done (SDP answer received)
    // Causes PCRF to install QCI=1 dedicated bearer via Gx RAR to P-GW
    DIA_RX_AAR = 0x0701,  // AA-Request: P-CSCF tells PCRF about media session
    DIA_RX_AAA = 0x0702,  // AA-Answer: PCRF confirms, triggers QCI=1 bearer
};

inline const char* sip_type_str(SipMsgType t) {
    switch(t) {
        case SipMsgType::SIP_REGISTER:      return "SIP REGISTER";
        case SipMsgType::SIP_INVITE:        return "SIP INVITE";
        case SipMsgType::SIP_ACK:           return "SIP ACK";
        case SipMsgType::SIP_BYE:           return "SIP BYE";
        case SipMsgType::SIP_CANCEL:        return "SIP CANCEL";
        case SipMsgType::SIP_100_TRYING:    return "SIP 100 Trying";
        case SipMsgType::SIP_180_RINGING:   return "SIP 180 Ringing";
        case SipMsgType::SIP_183_PROGRESS:  return "SIP 183 Session Progress";
        case SipMsgType::SIP_200_OK:        return "SIP 200 OK";
        case SipMsgType::DIA_CX_UAR:        return "Diameter Cx UAR";
        case SipMsgType::DIA_CX_UAA:        return "Diameter Cx UAA";
        case SipMsgType::DIA_CX_SAR:        return "Diameter Cx SAR";
        case SipMsgType::DIA_CX_SAA:        return "Diameter Cx SAA";
        case SipMsgType::DIA_RX_AAR:        return "Diameter Rx AAR";
        case SipMsgType::DIA_RX_AAA:        return "Diameter Rx AAA";
        default:                            return "UNKNOWN";
    }
}

// ── SIP / IMS TLV tags ────────────────────────────────────────
enum class SipTag : uint16_t {
    // SIP Headers (as TLV fields)
    SIP_FROM        = 0x0500,  // From: sip:+919...@ims.mnc010.mcc404.3gppnetwork.org
    SIP_TO          = 0x0501,  // To: sip:+919...@ims.mnc010.mcc404.3gppnetwork.org
    SIP_CONTACT     = 0x0502,  // Contact: sip:ue@10.0.0.1:5060
    SIP_CALL_ID     = 0x0503,  // Call-ID: unique per dialog
    SIP_CSEQ        = 0x0504,  // uint32 CSeq number
    SIP_VIA         = 0x0505,  // Via: SIP/2.0/TCP 127.0.0.1:5060
    SIP_EXPIRES     = 0x0506,  // uint32 registration expiry seconds
    SIP_IMPU        = 0x0507,  // IMS Public User Identity (tel URI or SIP URI)
    SIP_IMPI        = 0x0508,  // IMS Private User Identity (NAI format)
    SIP_SCSCF_NAME  = 0x0509,  // S-CSCF name returned by HSS
    SIP_SDP         = 0x050A,  // SDP body (codec, RTP port, IP)
    SIP_STATUS_CODE = 0x050B,  // uint16 response code
    SIP_REASON      = 0x050C,  // reason phrase string

    // Diameter Cx IEs
    CX_IMPU         = 0x0600,  // Public User Identity
    CX_IMPI         = 0x0601,  // Private User Identity
    CX_VISITED_NW   = 0x0602,  // Visited Network Identifier
    CX_SCSCF_NAME   = 0x0603,  // S-CSCF name

    // Diameter Rx IEs
    RX_IMPU         = 0x0700,  // User identity (for bearer)
    RX_MED_COMP     = 0x0701,  // Media-Component (codec, bandwidth, direction)
    RX_RESULT       = 0x0702,  // uint8 result code
};

// ── Simple SIP message writer (wraps our TLV MessageWriter) ──
class SipWriter {
public:
    SipWriter(SipMsgType type, uint32_t seq)
        : type_(type), seq_(seq) {}

    SipWriter& from   (const std::string& v) { fields_.push_back({SipTag::SIP_FROM,     v}); return *this; }
    SipWriter& to     (const std::string& v) { fields_.push_back({SipTag::SIP_TO,       v}); return *this; }
    SipWriter& contact(const std::string& v) { fields_.push_back({SipTag::SIP_CONTACT,  v}); return *this; }
    SipWriter& callId (const std::string& v) { fields_.push_back({SipTag::SIP_CALL_ID,  v}); return *this; }
    SipWriter& impu   (const std::string& v) { fields_.push_back({SipTag::SIP_IMPU,     v}); return *this; }
    SipWriter& impi   (const std::string& v) { fields_.push_back({SipTag::SIP_IMPI,     v}); return *this; }
    SipWriter& scscf  (const std::string& v) { fields_.push_back({SipTag::SIP_SCSCF_NAME,v}); return *this; }
    SipWriter& sdp    (const std::string& v) { fields_.push_back({SipTag::SIP_SDP,      v}); return *this; }
    SipWriter& reason (const std::string& v) { fields_.push_back({SipTag::SIP_REASON,   v}); return *this; }

    std::vector<uint8_t> build() {
        MessageWriter w(static_cast<MessageType>(static_cast<uint16_t>(type_)), seq_);
        for (auto& [tag, val] : fields_)
            w.writeStr(static_cast<Tag>(static_cast<uint16_t>(tag)), val);
        return w.frame();
    }

private:
    SipMsgType type_;
    uint32_t   seq_;
    std::vector<std::pair<SipTag, std::string>> fields_;
};

// ── SIP message reader — thin wrapper around MessageReader ────
class SipReader {
public:
    explicit SipReader(const std::vector<uint8_t>& buf) : buf_(buf), r_(buf_) {}

    SipMsgType  msgType() const { return static_cast<SipMsgType>(static_cast<uint16_t>(r_.msgType())); }
    uint32_t    seqNum()  const { return r_.seqNum(); }
    bool        hasMore()       { return r_.hasMore(); }
    void        skip()          { r_.skip(); }
    std::string readStr()       { return r_.readStr(); }

    bool peek(Tag& t, uint16_t& l) { return r_.peek(t, l); }

private:
    const std::vector<uint8_t> buf_;  // owns a copy so reference stays valid
    MessageReader r_;
};
