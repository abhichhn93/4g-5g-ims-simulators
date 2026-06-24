// ============================================================
// SAS STUB — Simulated Spectrum Access System
//
// Listens on TCP port 8800 for connections from the Domain Proxy.
//
// A real SAS (e.g. Google SAS, Federated Wireless, CommScope) is
// a cloud service accessed over HTTPS.  This stub uses plain TCP
// with length-prefixed JSON so there are zero TLS dependencies.
//
// Supported message types (WInnForum WINNF-TS-0016):
//   RegistrationRequest    → RegistrationResponse  (assigns cbsdId)
//   GrantRequest           → GrantResponse          (assigns grantId + channel)
//   HeartbeatRequest       → HeartbeatResponse      (refreshes transmit window)
//   RelinquishmentRequest  → RelinquishmentResponse (releases spectrum)
//   DeregistrationRequest  → DeregistrationResponse (removes CBSD record)
//
// Grant policy (simplified):
//   - Always approves registration
//   - Grants GAA-tier spectrum at the requested center frequency and
//     bandwidth (up to 20 MHz) with max EIRP capped at 30 dBm
//   - Heartbeat interval is 120 seconds
//   - Grant expire time is 3600 seconds
// ============================================================
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include "common/logger.h"
#include "common/wire.h"

static std::atomic<int> g_cbsdCounter{1};
static std::atomic<int> g_grantCounter{1};

// Build a CBRS-style "cbsdId" — in real SAS this is globally unique.
static std::string makeCbsdId() {
    return "CBSD-SAS-" + std::to_string(g_cbsdCounter++);
}
static std::string makeGrantId() {
    return "GRANT-" + std::to_string(g_grantCounter++);
}

// ─────────────────────────────────────────────────────────────
// Handle one Domain Proxy connection (runs in its own thread).
// In real CBRS the SAS would receive *batched* arrays from the DP;
// here we handle one message at a time for clarity.
// ─────────────────────────────────────────────────────────────
static void handleDpConnection(Socket dpSock) {
    Logger::sys("SAS: Domain Proxy connected");

    while (true) {
        std::string req;
        if (!recvMsg(dpSock, req)) {
            Logger::sys("SAS: Domain Proxy disconnected");
            break;
        }

        Logger::sas(Logger::Level::ENGINEER, "RX: " + req);

        std::string type = json::get(req, "type");
        std::string resp;

        if (type == "RegistrationRequest") {
            // ── Step 1: Registration ────────────────────────────────
            // SAS assigns a cbsdId.  Real SAS also checks FCC ID against
            // the FCC equipment authorization database (OET ELS).
            std::string fccId    = json::get(req, "fccId");
            std::string callSign = json::get(req, "callSign");
            std::string newId    = makeCbsdId();

            Logger::sas(Logger::Level::BEGINNER,
                "CBRS Step 1: Registration — FCC ID=" + fccId +
                " CallSign=" + callSign + " → assigned cbsdId=" + newId);

            Logger::sas(Logger::Level::INTERVIEW_T,
                "[CBRS SPEC] Registration validates: FCC equipment authorization, "
                "antenna height, category A (<30 dBm EIRP) or B (<47 dBm EIRP), "
                "location accuracy (<50 m).  SAS checks against PAL database and "
                "ESC sensor network before issuing grants.");

            resp = json::obj({
                {"type",     json::str("RegistrationResponse")},
                {"cbsdId",   json::str(newId)},
                {"response", json::obj({{"responseCode", json::num(0)},
                                        {"responseMessage", json::str("SUCCESS")}})}
            });

        } else if (type == "GrantRequest") {
            // ── Step 2: Spectrum Grant ──────────────────────────────
            // SAS finds a free channel and issues a grant.
            // Real SAS checks: PAL occupancy, incumbent protection zones
            // (ESC sensors for naval radar), and inter-CBSD interference.
            std::string cbsdId  = json::get(req, "cbsdId");
            std::string freqStr = json::get(req, "operationFrequencyMHz");
            std::string bwStr   = json::get(req, "operationBandwidthMHz");
            double freq = freqStr.empty() ? 3555.0 : std::stod(freqStr);
            double bw   = bwStr.empty()   ? 10.0   : std::stod(bwStr);
            if (bw > 20.0) bw = 20.0; // GAA cap

            std::string grantId = makeGrantId();

            Logger::sas(Logger::Level::BEGINNER,
                "CBRS Step 2: Grant — cbsdId=" + cbsdId +
                " freq=" + std::to_string(freq) + " MHz bw=" +
                std::to_string(bw) + " MHz → grantId=" + grantId);

            Logger::sas(Logger::Level::INTERVIEW_T,
                "[CBRS SPEC] Channel types: PAL (Priority Access License, 70 MHz "
                "block, licensed via FCC auction) vs GAA (General Authorized Access, "
                "opportunistic).  This stub always grants GAA.  maxEirp capped at "
                "30 dBm for Category A CBSDs per Part 96 rules.");

            resp = json::obj({
                {"type",                     json::str("GrantResponse")},
                {"cbsdId",                   json::str(cbsdId)},
                {"grantId",                  json::str(grantId)},
                {"channelType",              json::str("GAA")},
                {"operationFrequencyMHz",    json::flt(freq)},
                {"operationBandwidthMHz",    json::flt(bw)},
                {"maxEirp",                  json::num(30)},
                {"grantExpireTime",          json::num(3600)},
                {"heartbeatInterval",        json::num(120)},
                {"response",                 json::obj({{"responseCode", json::num(0)}})}
            });

        } else if (type == "HeartbeatRequest") {
            // ── Step 3: Heartbeat ────────────────────────────────────
            // CBSD must heartbeat every `heartbeatInterval` seconds or SAS
            // revokes the grant.  SAS uses this to detect dead/moved devices.
            std::string cbsdId  = json::get(req, "cbsdId");
            std::string grantId = json::get(req, "grantId");
            std::string opState = json::get(req, "operationState");

            Logger::sas(Logger::Level::BEGINNER,
                "CBRS Step 3: Heartbeat — cbsdId=" + cbsdId +
                " grantId=" + grantId + " state=" + opState);

            Logger::sas(Logger::Level::INTERVIEW_T,
                "[CBRS SPEC] heartbeatInterval keeps SAS aware of active transmitters. "
                "If the CBSD misses a heartbeat, SAS transitions the grant to TERMINATED "
                "and the CBSD must stop transmitting within 60 s (the Move List timer).");

            resp = json::obj({
                {"type",              json::str("HeartbeatResponse")},
                {"cbsdId",            json::str(cbsdId)},
                {"grantId",           json::str(grantId)},
                {"transmitExpireTime", json::num(3600)},
                {"heartbeatInterval", json::num(120)},
                {"response",          json::obj({{"responseCode", json::num(0)}})}
            });

        } else if (type == "RelinquishmentRequest") {
            // ── Step 4: Relinquishment ──────────────────────────────
            // CBSD voluntarily returns the grant before it expires.
            // Frees spectrum for other CBSDs immediately.
            std::string cbsdId  = json::get(req, "cbsdId");
            std::string grantId = json::get(req, "grantId");

            Logger::sas(Logger::Level::BEGINNER,
                "CBRS Step 4: Relinquishment — cbsdId=" + cbsdId +
                " grantId=" + grantId + " (spectrum freed)");

            resp = json::obj({
                {"type",     json::str("RelinquishmentResponse")},
                {"cbsdId",   json::str(cbsdId)},
                {"grantId",  json::str(grantId)},
                {"response", json::obj({{"responseCode", json::num(0)}})}
            });

        } else if (type == "DeregistrationRequest") {
            // ── Step 5: Deregistration ──────────────────────────────
            // CBSD is being decommissioned or moved.  SAS removes its record.
            std::string cbsdId = json::get(req, "cbsdId");

            Logger::sas(Logger::Level::BEGINNER,
                "CBRS Step 5: Deregistration — cbsdId=" + cbsdId + " removed");

            resp = json::obj({
                {"type",     json::str("DeregistrationResponse")},
                {"cbsdId",   json::str(cbsdId)},
                {"response", json::obj({{"responseCode", json::num(0)}})}
            });

        } else {
            Logger::warn("SAS", "Unknown message type: " + type);
            resp = json::obj({
                {"type",     json::str("ErrorResponse")},
                {"response", json::obj({{"responseCode", json::num(103)},
                                        {"responseMessage", json::str("INVALID_VALUE")}})}
            });
        }

        Logger::sas(Logger::Level::ENGINEER, "TX: " + resp);
        sendMsg(dpSock, resp);
    }
}

// ─────────────────────────────────────────────────────────────
// main — listen for Domain Proxy connections on port 8800
// ─────────────────────────────────────────────────────────────
int main() {
    Logger::setSessionFile("sas_session.log");
    Logger::setLevelFromEnv();

    Logger::step("SAS Stub starting — port 8800");
    Logger::sas(Logger::Level::BEGINNER,
        "Simulated Spectrum Access System ready. "
        "Waiting for Domain Proxy connection...");

    Logger::sas(Logger::Level::INTERVIEW_T,
        "[CBRS SPEC] Real SAS operators (Google, Federated Wireless, CommScope) "
        "must be certified by the FCC.  They protect Navy radar incumbents via "
        "ESC (Environmental Sensing Capability) sensors on the coasts.  All "
        "CBSD communication to SAS is mutually-authenticated TLS.");

    try {
        auto server = Socket::createServer("0.0.0.0", 8800);
        Logger::sys("SAS: listening on 0.0.0.0:8800");

        while (true) {
            auto client = server.accept();
            // spawn a thread per DP connection (typically only one DP per SAS zone)
            std::thread(handleDpConnection, std::move(client)).detach();
        }
    } catch (const std::exception& ex) {
        Logger::warn("SAS", std::string(ex.what()));
        return 1;
    }
    return 0;
}
