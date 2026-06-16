#include "common/nas_eps.h"
#include <string>

namespace nas_eps {

// First octet = digit1<<4 | odd/even(1)<<3 | type(001).
// Remaining 14 digits BCD-packed, low nibble = earlier digit.
std::vector<uint8_t> encodeEpsIdImsi(uint64_t imsi) {
    std::string digits = std::to_string(imsi); // 15 digits for our IMSI range
    std::vector<uint8_t> out;
    out.reserve(8);
    out.push_back(static_cast<uint8_t>(((digits[0] - '0') << 4) | (1 << 3) | 0b001));
    for (size_t i = 1; i + 1 < digits.size(); i += 2) {
        uint8_t lo = static_cast<uint8_t>(digits[i]   - '0');
        uint8_t hi = static_cast<uint8_t>(digits[i+1] - '0');
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out; // 8 bytes
}

std::vector<uint8_t> buildAttachRequest(uint64_t imsi) {
    std::vector<uint8_t> nas;
    nas.push_back(0x07);                 // Protocol discriminator = EMM
    nas.push_back(0x41);                 // Attach request
    nas.push_back((7 << 4) | 1);         // NAS key set identifier=7, EPS attach type=1
    auto epsid = encodeEpsIdImsi(imsi);
    nas.push_back(static_cast<uint8_t>(epsid.size()));
    nas.insert(nas.end(), epsid.begin(), epsid.end());
    nas.insert(nas.end(), {0x02, 0xe0, 0xe0}); // UE network capability
    static const std::vector<uint8_t> esm = {0x02, 0x01, 0xd0, 0x11}; // ESM PDN connectivity request
    nas.push_back(static_cast<uint8_t>((esm.size() >> 8) & 0xFF));
    nas.push_back(static_cast<uint8_t>(esm.size() & 0xFF));
    nas.insert(nas.end(), esm.begin(), esm.end());
    return nas; // 21 bytes
}

std::vector<uint8_t> buildAuthRequest(const uint8_t rand16[16], const uint8_t autn16[16]) {
    std::vector<uint8_t> nas;
    nas.push_back(0x07);
    nas.push_back(0x52);  // Authentication request
    nas.push_back(0x00);  // spare half octet | NAS key set identifier=0
    nas.insert(nas.end(), rand16, rand16 + 16);
    nas.push_back(0x10);  // AUTN length = 16
    nas.insert(nas.end(), autn16, autn16 + 16);
    return nas; // 36 bytes
}

std::vector<uint8_t> buildAuthResponse(const uint8_t res8[8]) {
    std::vector<uint8_t> nas = {0x07, 0x53, 0x08};
    nas.insert(nas.end(), res8, res8 + 8);
    return nas; // 11 bytes
}

const std::vector<uint8_t> SECURITY_MODE_COMMAND  = {0x07, 0x5d, 0x22, 0x07, 0x02, 0xe0, 0xe0};
const std::vector<uint8_t> SECURITY_MODE_COMPLETE = {0x07, 0x5e};
const std::vector<uint8_t> ATTACH_COMPLETE        = {0x07, 0x43, 0x00, 0x03, 0x52, 0x01, 0xc2};

std::vector<uint8_t> buildAttachAccept(const uint8_t ue_ip[4]) {
    std::vector<uint8_t> esm = {
        0x52, 0x01, 0xc1,             // ESM header: EBI=5, ProtDisc=2, MsgType=Activate Default EPS Bearer Ctx Req
        0x01, 0x09,                   // EPS QoS: QCI=9
        0x09, 0x08, 'i','n','t','e','r','n','e','t', // APN
        0x05, 0x01,                   // PDN address: type=IPv4
        ue_ip[0], ue_ip[1], ue_ip[2], ue_ip[3],
    };
    std::vector<uint8_t> nas = {
        0x07, 0x42,                   // Attach accept
        0x01,                         // EPS attach result=1 | spare
        0x1e,                         // T3412 value
        0x06,                         // TAI list length
        0x00, 0x04, 0xf4, 0x01, 0x00, 0x01, // TAI: PLMN=04f401 (MCC=404/MNC=10), TAC=1
    };
    nas.push_back(static_cast<uint8_t>((esm.size() >> 8) & 0xFF));
    nas.push_back(static_cast<uint8_t>(esm.size() & 0xFF));
    nas.insert(nas.end(), esm.begin(), esm.end());
    return nas; // 34 bytes
}

// ── TAU Request (TS 24.301 §8.2.29) ──────────────────────────
// Embedded in UL NAS Transport S1AP (eNB→MME).
// UE sends this when it moves to a new Tracking Area.
// Old GUTI/IMSI tells the MME which UE context to look up.
std::vector<uint8_t> buildTauRequest(uint64_t imsi) {
    std::vector<uint8_t> nas;
    nas.push_back(0x07);  // PD=7 (EMM), security header=0
    nas.push_back(0x48);  // Message type: Tracking area updating request
    nas.push_back(0x00);  // NAS KSI=0 (native, existing security ctx) | EPS update type=0 (TA updating)
    // Old GUTI encoded as IMSI (UE sends IMSI when no GUTI available, e.g. first attach)
    auto epsid = encodeEpsIdImsi(imsi);
    nas.push_back(static_cast<uint8_t>(epsid.size()));
    nas.insert(nas.end(), epsid.begin(), epsid.end());
    // UE network capability (reuse same as Attach Request)
    nas.insert(nas.end(), {0x58, 0x02, 0xe0, 0xe0}); // IEI=0x58, len=2, EEA2+EIA2
    return nas; // 16 bytes
}

// ── TAU Accept (TS 24.301 §8.2.28) ──────────────────────────
// MME sends this to confirm the TAU. UE updates its registered TAI.
std::vector<uint8_t> buildTauAccept() {
    return {
        0x07, 0x49,  // EMM, Tracking area updating accept
        0x00,        // EPS update result=0 (TA updated), Active flag=0
        0x5A, 0x49,  // T3412 value IE: IEI=0x5A, 0x49=360s*9 = 54 minutes (standard)
        0x54, 0x07,  // TAI list IEI=0x54, length=7
        0x00,        // Partial TAI list: type=0 (consecutive TACs, same PLMN)
        0x01,        // Number of elements = 2 (element count - 1 = 1)
        0x04, 0xF4, 0x01,  // PLMN = MCC=404/MNC=10 (coded as 04 F4 01)
        0x00, 0x02,  // TAC = 2 (new tracking area UE moved into)
    };
}

} // namespace nas_eps
