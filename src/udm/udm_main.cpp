// ============================================================
// UDM — Unified Data Management
//
// REAL ROLE (TS 23.501 §6.2.5): holds subscriber data (subscribed
// to UDR underneath) and runs the authentication-vector-generation
// part of 5G-AKA. AMF talks to it over the SBI (HTTP/2 + JSON):
//
//   Nudm_UEAuthentication_Get  (TS 29.509)
//     POST /nudm-ueau/v2/{suci}/security-information/generate-auth-data
//     -> RAND, AUTN, XRES*, KAUSF
//
//   Nudm_SDM_Get  (TS 29.503), "am-data" = Access & Mobility data
//     GET  /nudm-sdm/v2/{supi}/am-data
//     -> subscribed S-NSSAI(s), UE-AMBR
//
// THIS SIM: a tiny HTTP/1.1 server. One connection per request (no
// keep-alive) -- AMF opens a fresh TCP connection for each SBI call,
// which keeps both sides simple and gives each call its own clean
// pcap stream.
//
// 5G ~ 4G analogy: UDM is the 5G counterpart of the 4G simulator's
// HSS (src/hss/hss_node.cpp) -- subscriber DB + auth vectors.
// ============================================================
#include "common/socket_wrapper.h"
#include "common/wire.h"
#include "common/aka_lite.h"
#include "common/ids5g.h"
#include "common/logger.h"
#include "common/nrf_client.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

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

// Nudm_UEAuthentication_Get — generate a 5G-AKA challenge for this SUCI.
static std::string handleAuthDataRequest(const std::string& suci) {
    std::string msin = ids5g::msinFromSuci(suci);
    std::string supi = ids5g::supiFromSuci(suci);
    std::string k    = aka::kFor(msin);

    std::string rnd      = aka::randomHex(16);
    std::string autn     = aka::randomHex(16); // placeholder; real AUTN = SQN⊕AK || AMF || MAC
    std::string xresStar = aka::byteXor(rnd, k);
    std::string kausf    = aka::randomHex(32);

    Logger::sys("UDM: Nudm_UEAuthentication_Get for " + suci);
    Logger::udm(Logger::Level::ENGINEER, "Resolved " + suci + " -> SUPI " + supi);
    Logger::ie_field("K (long-term secret, never sent) = " + k);
    Logger::ie_field("RAND   = " + rnd);
    Logger::ie_field("AUTN   = " + autn);
    Logger::ie_field("XRES*  = " + xresStar + "  (AMF will compare UE's RES* against this)");
    Logger::ie_field("KAUSF  = " + kausf);
    Logger::udm(Logger::Level::INTERVIEW_T,
        "Null-scheme SUCI (TS 33.501 Annex C.2): scheme-output IS the MSIN in "
        "plaintext, so UDM can resolve SUCI->SUPI without decrypting anything.");

    std::string body = json::obj({
        {"authType", json::str("5G_AKA")},
        {"supi", json::str(supi)},
        {"rand", json::str(rnd)},
        {"autn", json::str(autn)},
        {"xresStar", json::str(xresStar)},
        {"kausf", json::str(kausf)},
    });
    return httpBuild("HTTP/1.1 200 OK", body);
}

// Nudm_SDM_Get (am-data) — subscribed slice + AMBR for this SUPI.
static std::string handleSdmRequest(const std::string& supi) {
    Logger::sys("UDM: Nudm_SDM_Get (am-data) for " + supi);
    Logger::ie_field("Subscribed S-NSSAI = {sst:1, sd:\"000001\"}  (eMBB slice)");
    Logger::ie_field("Subscribed-UE-AMBR = 100Mbps up / 200Mbps down");
    Logger::udm(Logger::Level::INTERVIEW_T,
        "am-data = 'Access and Mobility' subscription data -- AMF caches this "
        "after registration so it doesn't re-query UDM on every procedure.");

    std::string body = json::obj({
        {"supi", json::str(supi)},
        {"nssai", "{\"defaultSingleNssais\":[{\"sst\":1,\"sd\":\"000001\"}]}"},
        {"subscribedUeAmbr", "{\"uplink\":\"100Mbps\",\"downlink\":\"200Mbps\"}"},
    });
    return httpBuild("HTTP/1.1 200 OK", body);
}

int main() {
    Logger::setSessionFile("g5_udm_session.log");
    Logger::setLevelFromEnv();

    const uint16_t SBI_PORT = 29503;
    // UDM_SELF_HOST lets this binary tell the NRF where it can be reached:
    // locally "127.0.0.1", but a container/pod sets it to its own Docker
    // Compose service name / K8s Service DNS name (e.g. "udm-sim").
    const char* UDM_SELF_HOST = std::getenv("UDM_SELF_HOST") ? std::getenv("UDM_SELF_HOST") : "127.0.0.1";

    Logger::step("UDM starting");

    // Open the SBI listening socket FIRST, before any NRF round-trips: AMF's
    // callUdm() has no retry, so "connection refused" while UDM is still
    // busy registering with NRF would be an uncaught exception in AMF.
    Socket server = Socket::createServer("0.0.0.0", SBI_PORT);
    Logger::sys("UDM: Unified Data Management listening on :" + std::to_string(SBI_PORT));

    nrfclient::registerSelf(Logger::CLR_UDM, " UDM  ", "udm-1", "UDM", UDM_SELF_HOST, SBI_PORT);
    Logger::sys("UDM: PLMN " + ids5g::mcc() + "/" + ids5g::mnc() +
                 " -- test subscribers imsi-" + ids5g::mcc() + ids5g::mnc() +
                 "0000000001 .. 0000000099");

    while (true) {
        Socket client = server.accept();
        HttpMessage req;
        if (!httpRecv(client, req)) continue;

        std::istringstream ss(req.startLine);
        std::string method, path, ver;
        ss >> method >> path >> ver;

        auto parts = splitPath(path);
        std::string id = parts.size() > 2 ? parts[2] : "";

        std::string resp;
        if (path.find("/security-information/generate-auth-data") != std::string::npos) {
            resp = handleAuthDataRequest(id);
        } else if (path.find("/am-data") != std::string::npos) {
            resp = handleSdmRequest(id);
        } else {
            Logger::warn(" UDM  ", "unknown SBI path: " + path);
            resp = httpBuild("HTTP/1.1 404 Not Found", "{}");
        }
        httpSend(client, resp);
    }
}
