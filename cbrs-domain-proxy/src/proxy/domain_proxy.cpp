// ============================================================
// DOMAIN PROXY — CBRS middleware between CBSDs and the SAS
//
// Architecture:
//
//   CBSD-1 ──TCP:8700──► Domain Proxy ──TCP:8800──► SAS
//   CBSD-2 ──TCP:8700──►    (this file)
//   CBSD-N ──TCP:8700──►
//
// Role: The Domain Proxy aggregates multiple CBSDs and presents
// them to the SAS as a single entity.  A real DP runs at a
// cell tower or indoor deployment and may manage 10–100 CBSDs.
//
// What this file implements (matching the resume):
//   1. CBSD Registration handler      — validates and forwards RegistrationRequest
//   2. SAS grant request/response     — forwards GrantRequest, stores grant state
//   3. Spectrum allocation state machine — enforces valid transitions
//   4. TCP socket communication       — accept() loop + per-CBSD thread
//
// C++ design patterns used (Radisys interview topics):
//   - RAII:       Socket class closes fd in destructor
//   - Mutex:      CbsdRegistry protected by std::mutex (shared state)
//   - Thread pool: one thread per CBSD connection (std::thread::detach)
//   - State machine: SpectrumState enum enforced before SAS forward
// ============================================================
#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <chrono>
#include "common/logger.h"
#include "common/wire.h"
#include "proxy/cbsd_registry.h"
#include "proxy/spectrum_state.h"

static CbsdRegistry g_registry;
static std::mutex   g_sas_mtx;   // only one thread talks to SAS at a time
static Socket       g_sas_sock;  // persistent TCP connection to SAS

// ── Connect to SAS once at startup ─────────────────────────────────────────
static bool connectToSas(const char* host, uint16_t port) {
    try {
        g_sas_sock = Socket::connectTo(host, port);
        Logger::sys("DP: Connected to SAS at " + std::string(host) +
                    ":" + std::to_string(port));
        return true;
    } catch (const std::exception& ex) {
        Logger::warn("DP", std::string("Cannot reach SAS: ") + ex.what());
        return false;
    }
}

// ── Forward one JSON message to SAS, return the SAS response ───────────────
// Thread-safe: only one CBSD sends to SAS at a time (g_sas_mtx).
// Real DP batches multiple CBSD requests in one SAS HTTP call to reduce
// latency.  Here we serialize for simplicity.
static std::string forwardToSas(const std::string& req) {
    std::lock_guard<std::mutex> lk(g_sas_mtx);
    sendMsg(g_sas_sock, req);
    std::string resp;
    recvMsg(g_sas_sock, resp);
    return resp;
}

// ── Per-CBSD connection handler ──────────────────────────────────────────────
// Runs in its own thread.  Receives CBRS messages from one CBSD, validates
// the state machine transition, forwards to SAS, returns response.
static void handleCbsd(Socket cbsdSock) {
    std::string myCbsdId;  // assigned by SAS after registration

    Logger::dp(Logger::Level::BEGINNER,
        "New CBSD connected. Waiting for RegistrationRequest...");
    Logger::dp(Logger::Level::INTERVIEW_T,
        "[CBRS SPEC] CBSD must always register before requesting spectrum. "
        "The Domain Proxy enforces this order (state machine check) BEFORE "
        "forwarding to SAS — saves a round-trip to the cloud SAS server.");

    while (true) {
        std::string req;
        if (!recvMsg(cbsdSock, req)) {
            Logger::sys("DP: CBSD " + (myCbsdId.empty() ? "?" : myCbsdId) + " disconnected");
            if (!myCbsdId.empty()) g_registry.remove(myCbsdId);
            break;
        }

        std::string type = json::get(req, "type");
        Logger::dp(Logger::Level::ENGINEER, "← " + type +
                   (myCbsdId.empty() ? "" : "  [cbsdId=" + myCbsdId + "]"));

        // ── State machine guard ─────────────────────────────────────────────
        // Enforce legal transitions BEFORE talking to SAS.
        // This is the "spectrum allocation state machine" from the resume.
        CbsdInfo info;
        if (!myCbsdId.empty()) g_registry.get(myCbsdId, info);

        if (type == "GrantRequest" &&
            info.state != SpectrumState::REGISTERED) {
            Logger::warn("DP", "GrantRequest rejected: state=" +
                         stateToString(info.state) + " (must be REGISTERED first)");
            std::string err = json::obj({
                {"type",     json::str("GrantResponse")},
                {"response", json::obj({{"responseCode",    json::num(103)},
                                        {"responseMessage", json::str("Out-of-order: must be REGISTERED")}})}
            });
            sendMsg(cbsdSock, err);
            continue;
        }
        if (type == "HeartbeatRequest" &&
            info.state != SpectrumState::GRANTED &&
            info.state != SpectrumState::AUTHORIZED) {
            Logger::warn("DP", "Heartbeat rejected: no active grant");
            std::string err = json::obj({
                {"type",     json::str("HeartbeatResponse")},
                {"response", json::obj({{"responseCode",    json::num(400)},
                                        {"responseMessage", json::str("TERMINATED_GRANT")}})}
            });
            sendMsg(cbsdSock, err);
            continue;
        }

        // ── Forward to SAS ─────────────────────────────────────────────────
        Logger::dp(Logger::Level::ENGINEER, "→ SAS: " + type);
        std::string resp = forwardToSas(req);
        Logger::dp(Logger::Level::ENGINEER, "← SAS: " + resp);

        std::string respType = json::get(resp, "type");
        std::string rc       = json::get(resp, "responseCode");
        bool success = (rc == "0" || rc.empty());

        // ── Update local state after SAS response ──────────────────────────
        if (respType == "RegistrationResponse" && success) {
            myCbsdId = json::get(resp, "cbsdId");
            CbsdInfo newInfo;
            newInfo.cbsdId   = myCbsdId;
            newInfo.fccId    = json::get(req, "fccId");
            newInfo.callSign = json::get(req, "callSign");
            newInfo.category = json::get(req, "cbsdCategory");
            newInfo.state    = SpectrumState::REGISTERED;
            g_registry.upsert(newInfo);

            Logger::dp(Logger::Level::BEGINNER,
                "✓ CBSD registered: cbsdId=" + myCbsdId +
                "  total registered=" + std::to_string(g_registry.count()));
            Logger::ie_field("  fccId:    " + newInfo.fccId);
            Logger::ie_field("  callSign: " + newInfo.callSign);
            Logger::ie_field("  category: " + newInfo.category +
                             " (A=indoor ≤30dBm, B=outdoor ≤47dBm)");

        } else if (respType == "GrantResponse" && success) {
            std::string grantId = json::get(resp, "grantId");
            double freqMHz = 0, bwMHz = 0, maxEirp = 0;
            try { freqMHz = std::stod(json::get(resp, "operationFrequencyMHz")); } catch (...) {}
            try { bwMHz   = std::stod(json::get(resp, "operationBandwidthMHz")); } catch (...) {}
            try { maxEirp = std::stod(json::get(resp, "maxEirp")); } catch (...) {}
            g_registry.setGrant(myCbsdId, grantId, freqMHz, bwMHz, maxEirp);

            Logger::dp(Logger::Level::BEGINNER,
                "✓ Spectrum grant: " + std::to_string(freqMHz) + " MHz  BW=" +
                std::to_string(bwMHz) + " MHz  maxEirp=" + std::to_string(maxEirp) + " dBm");
            Logger::ie_field("  grantId:     " + grantId);
            Logger::ie_field("  channelType: GAA (General Authorized Access)");
            Logger::ie_field("  heartbeat:   every 120s or SAS revokes grant");
            Logger::dp(Logger::Level::INTERVIEW_T,
                "[CBRS SPEC] GAA is free-to-use spectrum. PAL holders (licensed via FCC "
                "auction) get priority. If a PAL holder shows up, SAS sends Move List to "
                "revoke GAA grants in that channel (ESC sensor triggers this).");

        } else if (respType == "HeartbeatResponse" && success) {
            g_registry.setState(myCbsdId, SpectrumState::AUTHORIZED);
            Logger::dp(Logger::Level::BEGINNER,
                "✓ Heartbeat OK — CBSD " + myCbsdId + " is AUTHORIZED (transmitting)");

        } else if (respType == "RelinquishmentResponse" && success) {
            g_registry.clearGrant(myCbsdId);
            Logger::dp(Logger::Level::BEGINNER,
                "✓ Grant relinquished — spectrum returned to SAS pool");

        } else if (respType == "DeregistrationResponse") {
            g_registry.remove(myCbsdId);
            Logger::dp(Logger::Level::BEGINNER,
                "✓ CBSD " + myCbsdId + " deregistered");
            sendMsg(cbsdSock, resp);
            break;
        }

        // Forward SAS response back to CBSD
        sendMsg(cbsdSock, resp);
    }
}

// ── STATUS command — print all known CBSDs ──────────────────────────────────
static void printStatus() {
    Logger::step("Domain Proxy STATUS — " + std::to_string(g_registry.count()) + " CBSD(s)");
    g_registry.forEach([](const CbsdInfo& c) {
        Logger::dp(Logger::Level::BEGINNER,
            "  " + c.cbsdId + "  state=" + stateToString(c.state) +
            (c.grantId.empty() ? "" :
             "  grant=" + c.grantId +
             "  freq=" + std::to_string(c.grantFreqMHz) + " MHz"));
    });
}

// ── main ────────────────────────────────────────────────────────────────────
int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Logger::setSessionFile("dp_session.log");
    Logger::setLevelFromEnv();

    std::cout <<
        "\n  +=========================================+\n"
        "  |  CBRS DOMAIN PROXY                     |\n"
        "  |  Listens for CBSDs  on port 8700        |\n"
        "  |  Connects to SAS    on port 8800        |\n"
        "  |  Commands: STATUS  QUIT                  |\n"
        "  +=========================================+\n\n";

    Logger::dp(Logger::Level::INTERVIEW_T,
        "[CBRS SPEC] Domain Proxy role: aggregates multiple CBSDs into one "
        "SAS connection. Enforces the state machine (UNREGISTERED→REGISTERED→"
        "GRANTED→AUTHORIZED) locally, batches requests to SAS (JSON arrays), "
        "and shields individual CBSDs from SAS connectivity issues.");

    // Connect to SAS
    if (!connectToSas("127.0.0.1", 8800)) {
        std::cerr << "  Start sas_stub first on port 8800!\n";
        return 1;
    }

    // Start CBSD listener
    auto server = Socket::createServer("0.0.0.0", 8700);
    Logger::sys("DP: Listening for CBSD connections on 0.0.0.0:8700");

    std::atomic<bool> stop{false};

    // Accept loop in background thread
    std::thread acceptThread([&]() {
        while (!stop.load()) {
            try {
                auto client = server.accept();
                std::thread(handleCbsd, std::move(client)).detach();
            } catch (...) { if (!stop.load()) Logger::warn("DP", "accept() error"); }
        }
    });
    acceptThread.detach();

    // CLI in main thread
    std::string line;
    std::cout << "dp> " << std::flush;
    while (std::getline(std::cin, line)) {
        if (line == "STATUS" || line == "status") {
            printStatus();
        } else if (line == "QUIT" || line == "quit") {
            break;
        } else if (!line.empty()) {
            std::cout << "  Commands: STATUS  QUIT\n";
        }
        std::cout << "dp> " << std::flush;
    }

    stop.store(true);
    Logger::shutdown();
    return 0;
}
