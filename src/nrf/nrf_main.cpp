// ============================================================
// NRF — Network Repository Function
//
// REAL ROLE (TS 23.501 §6.2.6, TS 29.510): the "phone book" of the
// 5G core. Every other NF (AMF, UDM, SMF, ...) REGISTERS its own
// profile (host/port + capabilities) with the NRF on startup, then
// DISCOVERS peer NFs by type instead of using hardcoded addresses.
// This is the foundation of the Service Based Architecture (SBA) --
// it's how a real network adds/removes/replaces NF instances
// without anyone needing static config of "where is the UDM".
//
//   Nnrf_NFManagement_NFRegister (TS 29.510 §5.2.2.2)
//     PUT /nnrf-nfm/v1/nf-instances/{nfInstanceId}
//     body: {nfInstanceId, nfType, host, port} -> 201 Created
//
//   Nnrf_NFDiscovery_Search (TS 29.510 §5.4.2.3.2)
//     GET /nnrf-disc/v1/nf-instances?target-nf-type=UDM
//     -> 200 OK with the matching NF's profile, or 404 if none
//
// THIS SIM: a tiny HTTP/1.1 server, same style as udm_main.cpp.
// SIMPLIFYING ASSUMPTION (called out at runtime via INTERVIEW_T):
// ONE registered instance per nfType -- a real NRF supports many
// instances per type plus filtering/load-based selection. wire.h's
// json:: helpers only parse flat objects, so discovery returns the
// single matching profile directly instead of a `nfInstances` array.
//
// NOTE on the pcap: this sim's pcap is written ONLY by amf_sim (the
// node that already "sees" both N2 and SBI traffic) -- two processes
// can't safely append to one pcap file. So in THIS increment, NRF
// register/discover traffic (both UDM->NRF and AMF->NRF) is NOT in
// 5g_capture.pcap -- it's fully visible in the terminal/session logs
// via Logger::nrf()/Logger::udm()/Logger::amf() (BEGINNER lines now
// always print, see logger.h). PcapWriter::IP_NRF=10.1.0.6 is reserved
// for a future increment that has AMF wrap its NRF calls with
// pcap().writeAppText() the same way it already does for UDM.
// ============================================================
#include "common/socket_wrapper.h"
#include "common/wire.h"
#include "common/logger.h"
#include <iostream>
#include <sstream>
#include <vector>
#include <map>
#include <string>

using Logger::Level;

static std::vector<std::string> splitPath(const std::string& path) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : path) {
        if (c == '/') { if (!cur.empty()) parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) parts.push_back(cur);
    return parts;
}

// Pulls "target-nf-type=UDM" out of "/nnrf-disc/v1/nf-instances?target-nf-type=UDM".
// Query strings aren't JSON, so this is separate from the json:: helpers.
static std::string getQueryParam(const std::string& path, const std::string& key) {
    size_t q = path.find('?');
    if (q == std::string::npos) return "";
    std::string query = path.substr(q + 1);
    std::string needle = key + "=";
    size_t p = query.find(needle);
    if (p == std::string::npos) return "";
    p += needle.size();
    size_t end = query.find('&', p);
    return query.substr(p, end - p);
}

struct NfProfile {
    std::string nfInstanceId;
    std::string nfType;
    std::string host;
    uint16_t    port;
};

// nfType -> profile. ONE instance per type (see header note above).
static std::map<std::string, NfProfile> g_profiles;

// Nnrf_NFManagement_NFRegister — PUT /nnrf-nfm/v1/nf-instances/{nfInstanceId}
static std::string handleRegister(const std::string& nfInstanceId, const std::string& body) {
    std::string nfType = json::get(body, "nfType");
    std::string host   = json::get(body, "host");
    uint16_t    port   = uint16_t(std::stoi(json::get(body, "port")));

    g_profiles[nfType] = NfProfile{nfInstanceId, nfType, host, port};

    Logger::step("NRF: NFRegister -- " + nfType);
    Logger::nrf(Level::BEGINNER, "NRF <- " + nfType + ": Nnrf_NFManagement_NFRegister (PUT)");
    Logger::ie_field("nfInstanceId = " + nfInstanceId);
    Logger::ie_field("nfType       = " + nfType);
    Logger::ie_field("host:port    = " + host + ":" + std::to_string(port));
    Logger::nrf(Level::INTERVIEW_T,
        "Real NRF (TS 29.510) stores a full NFProfile (capacity, sNssais, "
        "heartbeat timer, load...) and supports MANY instances per nfType "
        "with subscribe/notify on changes. This sim keeps ONE instance per "
        "nfType -- enough to demonstrate the register-then-discover pattern.");

    std::string respBody = json::obj({
        {"nfInstanceId", json::str(nfInstanceId)},
        {"nfType", json::str(nfType)},
    });
    Logger::nrf(Level::BEGINNER, "NRF -> " + nfType + ": 201 Created");
    return httpBuild("HTTP/1.1 201 Created", respBody);
}

// Nnrf_NFDiscovery_Search — GET /nnrf-disc/v1/nf-instances?target-nf-type=X
static std::string handleDiscover(const std::string& targetType) {
    Logger::step("NRF: NFDiscovery -- target-nf-type=" + targetType);
    Logger::nrf(Level::BEGINNER, "NRF <- Nnrf_NFDiscovery_Search (GET ?target-nf-type=" + targetType + ")");

    auto it = g_profiles.find(targetType);
    if (it == g_profiles.end()) {
        Logger::warn(" NRF  ", "no registered instance for nfType=" + targetType);
        return httpBuild("HTTP/1.1 404 Not Found", "{}");
    }
    const NfProfile& p = it->second;
    Logger::ie_field("found " + targetType + " @ " + p.host + ":" + std::to_string(p.port));
    Logger::nrf(Level::INTERVIEW_T,
        "Real Nnrf_NFDiscovery_Search returns SearchResult{nfInstances:[...]} "
        "(TS 29.510 §6.2.3.2), an array because multiple matches are possible. "
        "This sim returns the single matching profile flat, since wire.h's "
        "json:: helpers only parse flat objects (documented simplification).");

    std::string body = json::obj({
        {"nfInstanceId", json::str(p.nfInstanceId)},
        {"nfType", json::str(p.nfType)},
        {"host", json::str(p.host)},
        {"port", json::num(p.port)},
    });
    Logger::nrf(Level::BEGINNER, "NRF -> requester: 200 OK (" + targetType + " profile)");
    return httpBuild("HTTP/1.1 200 OK", body);
}

int main() {
    Logger::setSessionFile("g5_nrf_session.log");
    Logger::setLevelFromEnv();

    const uint16_t SBI_PORT = 29510;
    Socket server = Socket::createServer("0.0.0.0", SBI_PORT);

    Logger::step("NRF starting");
    Logger::sys("NRF: Network Repository Function listening on :" + std::to_string(SBI_PORT));
    Logger::sys("NRF: Nnrf_NFManagement_NFRegister (PUT) + Nnrf_NFDiscovery_Search (GET), one instance per nfType");

    while (true) {
        Socket client = server.accept();
        HttpMessage req;
        if (!httpRecv(client, req)) continue;

        std::istringstream ss(req.startLine);
        std::string method, path, ver;
        ss >> method >> path >> ver;

        std::string resp;
        if (method == "PUT" && path.find("/nnrf-nfm/v1/nf-instances/") != std::string::npos) {
            auto parts = splitPath(path.substr(0, path.find('?')));
            std::string nfInstanceId = parts.size() > 3 ? parts[3] : "";
            resp = handleRegister(nfInstanceId, req.body);
        } else if (method == "GET" && path.find("/nnrf-disc/v1/nf-instances") != std::string::npos) {
            resp = handleDiscover(getQueryParam(path, "target-nf-type"));
        } else {
            Logger::warn(" NRF  ", "unknown SBI request: " + method + " " + path);
            resp = httpBuild("HTTP/1.1 404 Not Found", "{}");
        }
        httpSend(client, resp);
    }
}
