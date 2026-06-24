#pragma once
#include <string>
#include <vector>
#include <sstream>
#include "common/socket_wrapper.h"

// ============================================================
// WIRE HELPERS — zero external dependencies.
//
// Transport: JSON over TCP with a 4-byte little-endian length
// prefix (same framing as the 4G/5G simulators in this repo).
//
//   [4B LE length][JSON text body]
//
// Real CBRS (WInnForum WINNF-TS-0016) uses HTTPS+JSON.
// This simulator strips TLS so we can run anywhere without
// certificates — same functional message flow, easier to learn.
// ============================================================

// ---- Frame helpers ----
inline std::vector<uint8_t> makeFrame(const std::string& json_text) {
    uint32_t len = uint32_t(json_text.size());
    std::vector<uint8_t> f;
    f.push_back(uint8_t(len));        f.push_back(uint8_t(len >> 8));
    f.push_back(uint8_t(len >> 16));  f.push_back(uint8_t(len >> 24));
    f.insert(f.end(), json_text.begin(), json_text.end());
    return f;
}

inline std::string frameToText(const std::vector<uint8_t>& payload) {
    return std::string(payload.begin(), payload.end());
}

// ---- Minimal flat-JSON helpers ----
namespace json {

inline std::string str(const std::string& s) { return "\"" + s + "\""; }
inline std::string num(long long n)           { return std::to_string(n); }
inline std::string flt(double v) {
    std::ostringstream ss; ss << v; return ss.str();
}

// Build a flat JSON object from key-value pairs.
// Values are inserted verbatim so you can pass str(), num(), flt(),
// or a nested json::obj() result directly.
// Example: obj({{"type", str("GrantRequest")}, {"freq", flt(3555.0)}})
//       -> {"type":"GrantRequest","freq":3555}
inline std::string obj(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out = "{";
    for (size_t i = 0; i < kv.size(); ++i) {
        out += "\"" + kv[i].first + "\":" + kv[i].second;
        if (i + 1 < kv.size()) out += ",";
    }
    out += "}";
    return out;
}

// Naive flat getter — finds "key": and returns the token up to ',' or '}'.
// Sufficient for the fixed, flat schemas this simulator uses.
inline std::string get(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = text.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    if (p < text.size() && text[p] == '"') {
        size_t end = text.find('"', p + 1);
        return end == std::string::npos ? "" : text.substr(p + 1, end - (p + 1));
    }
    size_t end = text.find_first_of(",}", p);
    return end == std::string::npos ? text.substr(p) : text.substr(p, end - p);
}

} // namespace json

// ---- Send / receive convenience wrappers ----
inline bool sendMsg(const Socket& sock, const std::string& json_text) {
    auto frame = makeFrame(json_text);
    return sock.sendFrame(frame);
}

inline bool recvMsg(const Socket& sock, std::string& json_text) {
    std::vector<uint8_t> payload;
    if (!sock.recvFrame(payload)) return false;
    json_text = frameToText(payload);
    return true;
}
