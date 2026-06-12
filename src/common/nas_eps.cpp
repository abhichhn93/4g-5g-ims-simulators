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

} // namespace nas_eps
