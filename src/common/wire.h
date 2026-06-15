#pragma once
#include <string>
#include <vector>
#include <map>
#include <sstream>
#include "common/socket_wrapper.h"

// ============================================================
// WIRE HELPERS — small, hand-rolled, no external libraries.
//
// Two transports are used in this simulator:
//
//   N2  (gNB <-> AMF)
//     JSON text wrapped in a 4-byte little-endian length prefix —
//     same framing style as the 4G simulator's S1AP-over-TCP
//     (Socket::sendFrame / recvFrame). Real N2 is NGAP over SCTP;
//     this is the same simplification mme_sim already uses.
//
//   SBI (AMF <-> UDM, and later SMF/UPF)
//     REAL HTTP/1.1 request/response lines, so Wireshark's
//     built-in "HTTP" dissector parses our traffic natively — this
//     actually matches the REAL 3GPP SBA design (TS 29.500: service
//     based interfaces ARE HTTP/2 + JSON).
//
// JSON itself is flat key/value objects only — built and parsed
// with plain string operations. That's enough for our fixed
// message schemas and keeps this file dependency-free.
// ============================================================

// ---- N2 framing: [4-byte LE length][JSON text] ----
inline std::vector<uint8_t> n2Frame(const std::string& json_text) {
    uint32_t len = uint32_t(json_text.size());
    std::vector<uint8_t> f;
    f.push_back(uint8_t(len));       f.push_back(uint8_t(len >> 8));
    f.push_back(uint8_t(len >> 16)); f.push_back(uint8_t(len >> 24));
    f.insert(f.end(), json_text.begin(), json_text.end());
    return f;
}
inline std::string n2Text(const std::vector<uint8_t>& payload) {
    return std::string(payload.begin(), payload.end());
}

// ---- Minimal flat-JSON helpers ----
namespace json {

// Wrap a string value in quotes: str("x") -> "\"x\""
inline std::string str(const std::string& s) { return "\"" + s + "\""; }
inline std::string num(long long n) { return std::to_string(n); }

// obj({{"key", str("value")}, {"count", num(5)}}) -> {"key":"value","count":5}
// Values are inserted verbatim, so nested JSON fragments can be
// passed straight through too.
inline std::string obj(const std::vector<std::pair<std::string, std::string>>& kv) {
    std::string out = "{";
    for (size_t i = 0; i < kv.size(); ++i) {
        out += "\"" + kv[i].first + "\":" + kv[i].second;
        if (i + 1 < kv.size()) out += ",";
    }
    out += "}";
    return out;
}

// Naive flat-object getter: finds "key": and returns the following
// quoted string (without quotes) or bare token up to the next ',' or '}'.
// Good enough for our fixed, flat message schemas.
inline std::string get(const std::string& text, const std::string& key) {
    std::string needle = "\"" + key + "\":";
    size_t p = text.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    if (p < text.size() && text[p] == '"') {
        size_t end = text.find('"', p + 1);
        return text.substr(p + 1, end - (p + 1));
    }
    size_t end = text.find_first_of(",}", p);
    return text.substr(p, end - p);
}

} // namespace json

// ---- Minimal HTTP/1.1 message (request or response) ----
struct HttpMessage {
    std::string startLine; // "GET /path HTTP/1.1" or "HTTP/1.1 200 OK"
    std::map<std::string, std::string> headers;
    std::string body;
};

// Build a full HTTP/1.1 message: start line + standard headers + JSON body.
inline std::string httpBuild(const std::string& startLine, const std::string& body,
                              const std::string& extraHeader = "") {
    std::ostringstream ss;
    ss << startLine << "\r\n";
    if (!extraHeader.empty()) ss << extraHeader << "\r\n";
    ss << "Content-Type: application/json\r\n";
    ss << "Content-Length: " << body.size() << "\r\n";
    ss << "\r\n" << body;
    return ss.str();
}

// Read one HTTP request or response off a TCP socket (blocking).
// Reads byte-by-byte until the blank line that ends the headers —
// simple, and fine for the small messages this simulator sends.
inline bool httpRecv(const Socket& sock, HttpMessage& msg) {
    std::string raw;
    char c;
    while (raw.size() < 4 || raw.compare(raw.size() - 4, 4, "\r\n\r\n") != 0) {
        if (!sock.recvAll(&c, 1)) return false;
        raw.push_back(c);
    }
    raw.resize(raw.size() - 4); // drop the trailing blank line
    std::istringstream hs(raw);
    std::getline(hs, msg.startLine);
    if (!msg.startLine.empty() && msg.startLine.back() == '\r') msg.startLine.pop_back();

    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string k = line.substr(0, colon);
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        msg.headers[k] = v;
    }

    size_t bodyLen = 0;
    auto it = msg.headers.find("Content-Length");
    if (it != msg.headers.end()) bodyLen = std::stoul(it->second);
    msg.body.resize(bodyLen);
    if (bodyLen > 0 && !sock.recvAll(msg.body.data(), uint32_t(bodyLen))) return false;
    return true;
}

inline bool httpSend(const Socket& sock, const std::string& raw) {
    return sock.sendAll(raw.data(), uint32_t(raw.size()));
}
