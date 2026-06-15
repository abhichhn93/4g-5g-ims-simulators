#pragma once
#include <string>
#include <vector>
#include <random>
#include <sstream>
#include <iomanip>

// ============================================================
// "5G-AKA, SIMPLIFIED"
//
// Real 5G-AKA (TS 33.501 §6.1.3, Annex A) derives RAND/AUTN/RES*/
// XRES*/KAUSF using the Milenage algorithm set (f1-f5, AES-128
// based) over a long-term secret K stored in the USIM and in
// UDM/ARPF. Implementing Milenage is a lot of code for not much
// extra learning value here — what matters for the call flow is
// the SHAPE of the exchange:
//
//   1. UDM/AMF picks a random RAND, derives XRES* from RAND+K
//   2. UE (gNB sim) derives RES* from the SAME RAND+K
//   3. AMF checks RES* == XRES* -> proves the UE holds the real K
//
// We implement step 1/2's "derive from RAND+K" as a plain
// byte-XOR. Wrong cryptographically, right *shape* — same
// challenge/response pattern an interviewer will recognise.
// ============================================================
namespace aka {

inline std::vector<uint8_t> fromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(uint8_t(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

inline std::string toHex(const std::vector<uint8_t>& v) {
    std::ostringstream ss;
    for (auto b : v) ss << std::hex << std::setw(2) << std::setfill('0') << int(b);
    return ss.str();
}

inline std::string randomHex(int bytes) {
    std::random_device rd;
    std::vector<uint8_t> v(bytes);
    for (auto& b : v) b = uint8_t(rd() & 0xFF);
    return toHex(v);
}

// XOR two equal-length (or repeating) hex strings byte-by-byte.
inline std::string byteXor(const std::string& a_hex, const std::string& b_hex) {
    auto a = fromHex(a_hex), b = fromHex(b_hex);
    std::vector<uint8_t> out(a.size());
    for (size_t i = 0; i < a.size(); ++i) out[i] = a[i] ^ b[i % b.size()];
    return toHex(out);
}

// Per-subscriber long-term key K (16 bytes / 32 hex chars).
// In real life K lives only in the USIM and UDM/ARPF and never
// crosses the network. Here both gnb_sim (the "UE") and udm_sim
// derive the same K for a given MSIN so RES* == XRES* — exactly
// like a real SIM and a real UDM agreeing on K out-of-band.
inline std::string kFor(const std::string& msin) {
    std::string base = "00112233445566778899aabbccddee0";
    int last = msin.empty() ? 0 : (msin.back() - '0');
    base.back() = "0123456789abcdef"[last & 0xF];
    return base; // 32 hex chars = 16 bytes
}

} // namespace aka
