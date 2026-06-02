#pragma once
#include <cstdint>
#include <vector>
#include <cstring>
#include <string>
#include "common/message_types.h"

// ============================================================
// TLV ENCODING — Type + Length + Value
//
// WHY TLV over packed structs?
//   - Adding a new IE = add a new tag. Old receivers SKIP unknown tags.
//   - No recompile-everything required when adding fields.
//   - This is how every real telecom protocol works:
//       S1AP: ASN.1 with criticality (reject/ignore/notify)
//       Diameter: AVP code(4B) + flags(1B) + length(3B) + vendor(4B) + value
//       GTPv2-C: 1-byte IE type + 2-byte length + 1-byte instance + value
//
// INTERVIEW Q: "How do you add a new field without breaking old nodes?"
// ANSWER: "TLV. Receivers skip unknown tags using the length field. Only
//   process what you understand. Criticality (like S1AP reject/ignore)
//   handles the mandatory-but-unknown case."
//
// OUR FORMAT (all little-endian):
//   TCP frame:  [4B payload_len][8B FrameHeader][TLV IEs...]
//   UDP frame:  [8B FrameHeader][TLV IEs...]  (no prefix — UDP has own framing)
//   Each TLV:   [2B tag LE][2B value_len LE][value_len bytes]
// ============================================================

enum class Tag : uint16_t {
    // ── S1AP IEs (0x0010 – 0x004F) ──────────────────────────────────────
    ENB_UE_S1AP_ID  = 0x0010,  // uint32  eNB's UE handle
    MME_UE_S1AP_ID  = 0x0011,  // uint32  MME's UE handle
    RRC_CAUSE       = 0x0012,  // uint8   0=emergency,3=mo-Signalling,4=mo-Data
    TAI_MCC         = 0x0013,  // uint16  Mobile Country Code
    TAI_MNC         = 0x0014,  // uint16  Mobile Network Code
    TAI_TAC         = 0x0015,  // uint16  Tracking Area Code
    EUTRAN_CGI      = 0x0016,  // uint32  Cell Global Identity

    // ── S1AP ICSR / ICSRSP IEs (0x0050 – 0x007F) ────────────────────────
    ICSR_AMBR_UL    = 0x0050,  // uint32  Aggregate Max Bit Rate UL (bps)
    ICSR_AMBR_DL    = 0x0051,  // uint32  Aggregate Max Bit Rate DL (bps)
    ICSR_SGW_S1U_IP = 0x0052,  // 4B      S-GW's S1-U IP (eNB builds GTP-U tunnel to this)
    ICSR_SGW_TEID   = 0x0053,  // uint32  S-GW's S1-U TEID (GTP-U downlink tunnel)
    ICSR_ENB_TEID   = 0x0054,  // uint32  eNB's S1-U TEID (in ICSRSP, MME uses in Modify Bearer)

    // ── NAS IEs (0x0100 – 0x01FF) ────────────────────────────────────────
    NAS_PROTO_DISC  = 0x0100,  // uint8   0x07=EMM, 0x0A=ESM
    NAS_SEC_HDR     = 0x0101,  // uint8   0x00=plain
    NAS_MSG_TYPE    = 0x0102,  // uint8   0x41=AttachReq,0x42=Accept,0x46=Complete
                               //         0x52=AuthReq,0x53=AuthResp
    NAS_ATTACH_TYPE = 0x0103,  // uint8   1=EPS only
    NAS_KSI         = 0x0104,  // uint8   7=no key
    NAS_ID_TYPE     = 0x0105,  // uint8   1=IMSI,6=GUTI
    NAS_IMSI        = 0x0106,  // uint64  15-digit IMSI
    NAS_UE_CAP      = 0x0107,  // uint8   security capability bitmask
    NAS_RAND        = 0x0108,  // 16B     random challenge
    NAS_AUTN        = 0x0109,  // 16B     auth token
    NAS_RES         = 0x010A,  // 8B      UE's auth response
    NAS_EBI         = 0x010B,  // uint8   EPS Bearer ID (5=default)
    NAS_UE_IP       = 0x010C,  // 4B      UE's IPv4 address (in Attach Accept)

    // ── Diameter S6a IEs (0x0200 – 0x02FF) ──────────────────────────────
    DIA_IMSI        = 0x0200,  // uint64
    DIA_PLMN        = 0x0201,  // uint32
    DIA_RAND        = 0x0202,  // 16B
    DIA_AUTN        = 0x0203,  // 16B
    DIA_XRES        = 0x0204,  // 8B
    DIA_KASME       = 0x0205,  // 32B

    // ── GTP-C IEs (0x0300 – 0x03FF) — TS 29.274 ────────────────────────
    GTP_IMSI        = 0x0300,  // uint64  IMSI for HSS lookup / bearer context
    GTP_CAUSE       = 0x0301,  // uint8   16=Success (TS 29.274 §8.4)
    GTP_APN         = 0x0302,  // bytes   Access Point Name string
    GTP_PDN_TYPE    = 0x0303,  // uint8   1=IPv4
    GTP_EBI         = 0x0304,  // uint8   EPS Bearer ID (5=default bearer)
    GTP_QCI         = 0x0305,  // uint8   9=best-effort internet, 1=VoLTE (Phase 4)
    GTP_SENDER_TEID = 0x0306,  // uint32  Sender's GTP-C TEID for control plane
    GTP_BEARER_TEID = 0x0307,  // uint32  Assigned data-plane TEID (S1-U or S5-U)
    GTP_UE_IP       = 0x0308,  // 4B      UE IPv4 address (allocated by P-GW)
    GTP_ENB_TEID    = 0x0309,  // uint32  eNB's S1-U TEID (sent in Modify Bearer Req)
    GTP_AMBR_UL     = 0x030A,  // uint32  Aggregate Max Bit Rate UL
    GTP_AMBR_DL     = 0x030B,  // uint32  Aggregate Max Bit Rate DL

    // ── Diameter Gx IEs (0x0400 – 0x04FF) — TS 29.212 ────────────────────
    GX_IMSI         = 0x0400,  // uint64  Subscriber IMSI
    GX_APN          = 0x0401,  // bytes   Access Point Name (UTF-8)
    GX_QCI          = 0x0402,  // uint8   QoS Class Indicator
    GX_RULE_NAME    = 0x0403,  // bytes   Charging-Rule-Name (UTF-8)
    GX_MAX_UL_BPS   = 0x0404,  // uint32  Max UL bitrate (bps)
    GX_MAX_DL_BPS   = 0x0405,  // uint32  Max DL bitrate (bps)
    GX_RESULT_CODE  = 0x0406,  // uint8   1=SUCCESS (simplified Diameter result)
};

// ── Frame header: 8 bytes after the 4-byte TCP length prefix ─
struct FrameHeader {
    uint16_t msg_type;
    uint16_t flags;
    uint32_t seq_num;
};
static_assert(sizeof(FrameHeader) == 8, "FrameHeader must be 8 bytes");

// ============================================================
// MessageWriter — builds a TLV-encoded frame
// ============================================================
class MessageWriter {
public:
    MessageWriter(MessageType type, uint32_t seq, uint16_t flags = 0) {
        buf_.reserve(256);
        buf_.resize(4);                         // TCP length prefix placeholder
        pu16(static_cast<uint16_t>(type));
        pu16(flags);
        pu32(seq);
    }

    void writeU8   (Tag t, uint8_t  v)                           { th(t,1); buf_.push_back(v); }
    void writeU16  (Tag t, uint16_t v)                           { th(t,2); pu16(v); }
    void writeU32  (Tag t, uint32_t v)                           { th(t,4); pu32(v); }
    void writeU64  (Tag t, uint64_t v)                           { th(t,8); pu32(uint32_t(v)); pu32(uint32_t(v>>32)); }
    void writeBytes(Tag t, const uint8_t* d, uint16_t n)         { th(t,n); buf_.insert(buf_.end(),d,d+n); }
    void writeStr  (Tag t, const std::string& s)                 { writeBytes(t,reinterpret_cast<const uint8_t*>(s.data()),uint16_t(s.size())); }
    void writeIPv4 (Tag t, uint32_t ip_be)                       { th(t,4); pu32(ip_be); }

    // TCP: [4B len][header+TLVs]
    const std::vector<uint8_t>& frame() {
        uint32_t n = uint32_t(buf_.size())-4;
        buf_[0]=n; buf_[1]=n>>8; buf_[2]=n>>16; buf_[3]=n>>24;
        return buf_;
    }

    // UDP: [header+TLVs] without length prefix
    std::vector<uint8_t> udpPayload() const {
        return std::vector<uint8_t>(buf_.begin()+4, buf_.end());
    }

private:
    std::vector<uint8_t> buf_;
    void th(Tag t, uint16_t n) { uint16_t x=uint16_t(t); buf_.push_back(x); buf_.push_back(x>>8); buf_.push_back(n); buf_.push_back(n>>8); }
    void pu16(uint16_t v) { buf_.push_back(v); buf_.push_back(v>>8); }
    void pu32(uint32_t v) { buf_.push_back(v); buf_.push_back(v>>8); buf_.push_back(v>>16); buf_.push_back(v>>24); }
};

// ============================================================
// MessageReader — decodes a TLV frame
// ============================================================
class MessageReader {
public:
    // TCP payload (already stripped of 4-byte length prefix by recvFrame)
    explicit MessageReader(const std::vector<uint8_t>& buf) : buf_(buf), pos_(0) {
        if (buf_.size() >= 8) {
            msg_type_ = MessageType(gu16()); flags_ = gu16(); seq_num_ = gu32();
        }
    }

    MessageType msgType() const { return msg_type_; }
    uint16_t    flags()   const { return flags_; }
    uint32_t    seqNum()  const { return seq_num_; }
    bool        hasMore() const { return pos_ < buf_.size(); }

    bool peek(Tag& tag, uint16_t& len) const {
        if (pos_+4 > buf_.size()) return false;
        tag = Tag(buf_[pos_] | (uint16_t(buf_[pos_+1])<<8));
        len = uint16_t(buf_[pos_+2] | (uint16_t(buf_[pos_+3])<<8));
        return true;
    }

    // Each readXxx() skips the 4-byte TLV header, then reads the value
    uint8_t  readU8()  { pos_+=4; return buf_[pos_++]; }
    uint16_t readU16() { pos_+=4; uint16_t v=uint16_t(buf_[pos_])|(uint16_t(buf_[pos_+1])<<8); pos_+=2; return v; }
    uint32_t readU32() { pos_+=4; uint32_t v=buf_[pos_]|(buf_[pos_+1]<<8)|(buf_[pos_+2]<<16)|(buf_[pos_+3]<<24); pos_+=4; return v; }
    uint64_t readU64() {
        pos_+=4;
        uint64_t lo=buf_[pos_]|(buf_[pos_+1]<<8)|(buf_[pos_+2]<<16)|(buf_[pos_+3]<<24);
        uint64_t hi=buf_[pos_+4]|(buf_[pos_+5]<<8)|(buf_[pos_+6]<<16)|(buf_[pos_+7]<<24);
        pos_+=8; return lo|(hi<<32);
    }
    std::vector<uint8_t> readBytes() {
        Tag t; uint16_t len; peek(t,len); pos_+=4;
        std::vector<uint8_t> v(buf_.begin()+pos_, buf_.begin()+pos_+len);
        pos_+=len; return v;
    }
    std::string readStr() { auto b=readBytes(); return std::string(b.begin(),b.end()); }
    void skip() { Tag t; uint16_t len; if(peek(t,len)) pos_+=4+len; }

private:
    const std::vector<uint8_t>& buf_;
    size_t pos_{0};
    MessageType msg_type_{};
    uint16_t    flags_{0};
    uint32_t    seq_num_{0};

    // Internal helpers for FrameHeader parsing (no TLV header to skip)
    uint16_t gu16() { uint16_t v=uint16_t(buf_[pos_])|(uint16_t(buf_[pos_+1])<<8); pos_+=2; return v; }
    uint32_t gu32() { uint32_t v=buf_[pos_]|(buf_[pos_+1]<<8)|(buf_[pos_+2]<<16)|(buf_[pos_+3]<<24); pos_+=4; return v; }
};
