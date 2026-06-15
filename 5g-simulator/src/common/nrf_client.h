#pragma once
#include "common/socket_wrapper.h"
#include "common/wire.h"
#include "common/logger.h"
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>
#include <stdexcept>

// ============================================================
// NRF CLIENT — shared "register with NRF, then discover a peer"
// helper used by every NF in this sim (UDM, AMF, and future
// SMF/UPF). This is the actual SBA pattern the user asked to see:
// no node hardcodes a peer's address; everyone looks it up via NRF.
//
//   Nnrf_NFManagement_NFRegister  PUT /nnrf-nfm/v1/nf-instances/{id}
//   Nnrf_NFDiscovery_Search       GET /nnrf-disc/v1/nf-instances?target-nf-type=X
//
// NRF_HOST env var (default "127.0.0.1") points at nrf_sim,
// same pattern as UDM_HOST/AMF_HOST.
// ============================================================
namespace nrfclient {

constexpr uint16_t NRF_PORT = 29510;

inline std::string nrfHost() {
    const char* h = std::getenv("NRF_HOST");
    return h ? h : "127.0.0.1";
}

// Registers this NF's profile with the NRF. Retries ~10x 1s apart, since
// under Docker Compose/K8s the NRF and this NF can start in either order.
// On exhaustion, logs a SYSTEM warning and returns -- the sim degrades
// gracefully (a later discover() will just 404, visibly logged) instead
// of crashing.
inline void registerSelf(const char* color, const char* tag,
                          const std::string& nfInstanceId, const std::string& nfType,
                          const std::string& selfHost, uint16_t selfPort) {
    std::string body = json::obj({
        {"nfInstanceId", json::str(nfInstanceId)},
        {"nfType", json::str(nfType)},
        {"host", json::str(selfHost)},
        {"port", json::num(selfPort)},
    });
    std::string req = httpBuild("PUT /nnrf-nfm/v1/nf-instances/" + nfInstanceId + " HTTP/1.1", body);

    Logger::step(nfType + " registering with NRF");
    for (int attempt = 1; attempt <= 10; ++attempt) {
        try {
            Socket nrf = Socket::connectTo(nrfHost().c_str(), NRF_PORT);
            Logger::print(Logger::Level::BEGINNER, color, tag,
                "-> NRF: Nnrf_NFManagement_NFRegister (PUT, nfType=" + nfType + ")");
            Logger::ie_field("nfInstanceId = " + nfInstanceId);
            Logger::ie_field("host:port    = " + selfHost + ":" + std::to_string(selfPort));
            httpSend(nrf, req);
            HttpMessage resp;
            httpRecv(nrf, resp);
            Logger::print(Logger::Level::BEGINNER, color, tag, "<- NRF: " + resp.startLine);
            return;
        } catch (const std::exception&) {
            if (attempt == 10) {
                Logger::warn(tag, "could not reach NRF at " + nrfHost() + ":" +
                                   std::to_string(NRF_PORT) + " after 10 attempts -- "
                                   "continuing without registration");
                return;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

struct Discovered { std::string host; uint16_t port; bool found; };

// Discovers the single registered instance of `targetNfType` via NRF.
// Returns found=false on 404 or any connection error (logged as a
// SYSTEM warning) -- callers should fall back to their own default.
inline Discovered discover(const char* color, const char* tag, const std::string& targetNfType) {
    try {
        Socket nrf = Socket::connectTo(nrfHost().c_str(), NRF_PORT);
        std::string req = httpBuild("GET /nnrf-disc/v1/nf-instances?target-nf-type=" + targetNfType + " HTTP/1.1", "");
        Logger::print(Logger::Level::BEGINNER, color, tag,
            "-> NRF: Nnrf_NFDiscovery_Search (target-nf-type=" + targetNfType + ")");
        httpSend(nrf, req);
        HttpMessage resp;
        httpRecv(nrf, resp);
        if (resp.startLine.find("200") == std::string::npos) {
            Logger::warn(tag, "NRF discovery for " + targetNfType + " returned: " + resp.startLine);
            return {"", 0, false};
        }
        std::string host = json::get(resp.body, "host");
        uint16_t port    = uint16_t(std::stoi(json::get(resp.body, "port")));
        Logger::print(Logger::Level::BEGINNER, color, tag,
            "<- NRF: " + targetNfType + " is at " + host + ":" + std::to_string(port));
        Logger::ie_field("discovered " + targetNfType + " @ " + host + ":" + std::to_string(port) +
                         "  (via NRF -- not hardcoded)");
        return {host, port, true};
    } catch (const std::exception& e) {
        Logger::warn(tag, "NRF discovery for " + targetNfType + " failed: " + std::string(e.what()));
        return {"", 0, false};
    }
}

} // namespace nrfclient
