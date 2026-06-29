// ============================================================
// DOMAIN PROXY — CBRS middlebox between CBSDs and the SAS
//
// THREAD MAP (read this first):
//
//   [main thread]        — CLI (STATUS / QUIT), startup, named "main"
//   [accept thread]      — accept() loop, one per process, named "accept"
//   [cbsd-N thread]      — one per connected CBSD, named "cbsd-{cbsdId}"
//                          before registration: "cbsd-conn{N}"
//
// SHARED STATE (who touches what):
//
//   g_registry      — all cbsd threads read+write (protected by registry mutex)
//   g_sas_sock      — all cbsd threads use it (protected by g_sas_mtx)
//   g_sas_mtx       — serialises SAS socket: only ONE cbsd thread talks to
//                     SAS at a time (one TCP connection, not thread-safe)
//
// WHY ONE SAS CONNECTION?
//   Real DPs batch multiple CBSDs into one HTTPS call (JSON arrays).
//   Here we keep one TCP socket and serialise with a mutex — simpler to
//   learn, same correctness.  The mutex wait is visible in the logs.
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

static CbsdRegistry      g_registry;
static std::mutex         g_sas_mtx;
static Socket             g_sas_sock;
static std::atomic<int>   g_conn_counter{0};  // gives each thread a short numeric name

// ── Connect to SAS once at startup (called from main thread) ────────────────
static bool connectToSas(const char* host, uint16_t port) {
    Logger::sys("Connecting to SAS at " + std::string(host) + ":" + std::to_string(port) + " ...");
    try {
        g_sas_sock = Socket::connectTo(host, port);
        Logger::sys("Connected to SAS ✓  (persistent TCP — shared by all CBSD threads)");
        Logger::sys("g_sas_sock shared state: all cbsd-N threads use this one socket");
        Logger::sys("g_sas_mtx: ensures only ONE cbsd-N thread sends/receives at a time");
        return true;
    } catch (const std::exception& ex) {
        Logger::warn("DP", std::string("Cannot reach SAS: ") + ex.what());
        return false;
    }
}

// ── Forward one message to SAS and wait for the response ───────────────────
// Called from a cbsd-N thread.  Acquires g_sas_mtx so no other cbsd thread
// can interleave its own SAS send/receive while we're mid-conversation.
static std::string forwardToSas(const std::string& req, const std::string& msgType) {
    Logger::dp(Logger::Level::ENGINEER,
        "⟳  waiting for g_sas_mtx (other cbsd threads may be using SAS)...");

    std::lock_guard<std::mutex> lk(g_sas_mtx);
    // ── inside the lock ────────────────────────────────────────────────────
    Logger::dp(Logger::Level::ENGINEER,
        "✓  g_sas_mtx acquired — sending " + msgType + " to SAS");
    Logger::dp(Logger::Level::ENGINEER, "──────────────────────────► SAS: " + msgType);

    sendMsg(g_sas_sock, req);

    std::string resp;
    recvMsg(g_sas_sock, resp);

    std::string respType = json::get(resp, "type");
    Logger::dp(Logger::Level::ENGINEER, "◄────────────────────────── SAS: " + respType);
    Logger::dp(Logger::Level::ENGINEER,
        "✓  g_sas_mtx releasing — next cbsd thread can use SAS");
    // ── lock released here (lock_guard destructor) ─────────────────────────
    return resp;
}

// ── Per-CBSD connection handler — runs in its own thread ───────────────────
static void handleCbsd(Socket cbsdSock, int connId) {
    // Name this thread so it appears in all log lines while it runs.
    // Before registration we use "cbsd-conn1", "cbsd-conn2" etc.
    // After SAS assigns a cbsdId we rename to "cbsd-CBSD-SAS-1" etc.
    std::string threadLabel = "cbsd-conn" + std::to_string(connId);
    Logger::setThreadName(threadLabel);

    std::string myCbsdId;

    Logger::sys("Thread started — waiting for first message from CBSD");
    Logger::sys("This thread handles exactly ONE CBSD for its entire lifetime");

    while (true) {
        // ── Receive next message from the CBSD ───────────────────────────
        std::string req;
        if (!recvMsg(cbsdSock, req)) {
            std::string who = myCbsdId.empty() ? threadLabel : myCbsdId;
            Logger::sys("CBSD " + who + " disconnected (recv returned false)");
            if (!myCbsdId.empty()) {
                g_registry.remove(myCbsdId);
                Logger::sys("Removed " + myCbsdId + " from registry");
            }
            break;
        }

        std::string type = json::get(req, "type");
        Logger::dp(Logger::Level::ENGINEER,
            "◄── CBSD sent: " + type +
            (myCbsdId.empty() ? "" : "  [cbsdId=" + myCbsdId + "]"));

        // ── Read the current state for this CBSD ─────────────────────────
        CbsdInfo info;
        if (!myCbsdId.empty()) g_registry.get(myCbsdId, info);
        std::string curState = stateToString(info.state);

        // ── STATE MACHINE GUARD ──────────────────────────────────────────
        // Reject illegal transitions BEFORE contacting SAS.
        // This saves a round-trip to the cloud if the CBSD is out-of-order.
        if (type == "GrantRequest" && info.state != SpectrumState::REGISTERED) {
            Logger::warn("DP",
                "GrantRequest REJECTED — current state=" + curState +
                " (must be REGISTERED).  Not forwarding to SAS.");
            std::string err = json::obj({
                {"type",     json::str("GrantResponse")},
                {"response", json::obj({{"responseCode",    json::num(103)},
                                        {"responseMessage", json::str("Must be REGISTERED first")}})}
            });
            sendMsg(cbsdSock, err);
            Logger::dp(Logger::Level::ENGINEER, "──► CBSD: GrantResponse [error, not forwarded]");
            continue;
        }
        if (type == "HeartbeatRequest" &&
            info.state != SpectrumState::GRANTED &&
            info.state != SpectrumState::AUTHORIZED) {
            Logger::warn("DP",
                "HeartbeatRequest REJECTED — no active grant (state=" + curState + ")");
            std::string err = json::obj({
                {"type",     json::str("HeartbeatResponse")},
                {"response", json::obj({{"responseCode",    json::num(400)},
                                        {"responseMessage", json::str("TERMINATED_GRANT")}})}
            });
            sendMsg(cbsdSock, err);
            Logger::dp(Logger::Level::ENGINEER, "──► CBSD: HeartbeatResponse [error]");
            continue;
        }

        // ── Print fields of the incoming message ──────────────────────────
        if (type == "RegistrationRequest") {
            Logger::dp(Logger::Level::ENGINEER, "  Fields received from CBSD:");
            Logger::ie_field("fccId:          " + json::get(req, "fccId") +
                             "  ← FCC equipment authorization ID");
            Logger::ie_field("callSign:       " + json::get(req, "callSign") +
                             "  ← FCC operator call sign");
            Logger::ie_field("cbsdCategory:   " + json::get(req, "cbsdCategory") +
                             "  ← A=indoor(≤30dBm)  B=outdoor(≤47dBm)");
            Logger::ie_field("radioTechnology:" + json::get(req, "radioTechnology"));
            Logger::ie_field("latitude:       " + json::get(req, "latitude"));
            Logger::ie_field("longitude:      " + json::get(req, "longitude"));
            Logger::ie_field("height:         " + json::get(req, "height") + " m AGL");
            Logger::ie_field("antennaGain:    " + json::get(req, "antennaGain") + " dBi");
        } else if (type == "GrantRequest") {
            Logger::dp(Logger::Level::ENGINEER, "  Fields received from CBSD:");
            Logger::ie_field("cbsdId:                  " + json::get(req, "cbsdId"));
            Logger::ie_field("operationFrequencyMHz:   " + json::get(req, "operationFrequencyMHz") +
                             " MHz  ← center frequency requested");
            Logger::ie_field("operationBandwidthMHz:   " + json::get(req, "operationBandwidthMHz") +
                             " MHz  ← bandwidth requested");
            Logger::ie_field("maxEirp:                 " + json::get(req, "maxEirp") +
                             " dBm  ← transmit power limit");
        } else if (type == "HeartbeatRequest") {
            Logger::dp(Logger::Level::ENGINEER, "  Fields received from CBSD:");
            Logger::ie_field("cbsdId:         " + json::get(req, "cbsdId"));
            Logger::ie_field("grantId:        " + json::get(req, "grantId"));
            Logger::ie_field("operationState: " + json::get(req, "operationState") +
                             "  ← GRANTED or AUTHORIZED");
        } else if (type == "RelinquishmentRequest") {
            Logger::dp(Logger::Level::ENGINEER, "  Fields:");
            Logger::ie_field("cbsdId:  " + json::get(req, "cbsdId"));
            Logger::ie_field("grantId: " + json::get(req, "grantId") +
                             "  ← spectrum being returned");
        } else if (type == "DeregistrationRequest") {
            Logger::dp(Logger::Level::ENGINEER, "  Fields:");
            Logger::ie_field("cbsdId: " + json::get(req, "cbsdId") +
                             "  ← will be removed from SAS database");
        }

        // ── Forward to SAS (acquires g_sas_mtx inside) ───────────────────
        std::string resp = forwardToSas(req, type);

        // ── Print fields of the SAS response ─────────────────────────────
        std::string respType = json::get(resp, "type");
        std::string rc       = json::get(resp, "responseCode");
        bool success = (rc == "0" || rc.empty());

        Logger::dp(Logger::Level::ENGINEER, "  Fields in SAS response:");

        if (respType == "RegistrationResponse") {
            Logger::ie_field("cbsdId:       " + json::get(resp, "cbsdId") +
                             "  ← assigned by SAS for this session");
            Logger::ie_field("responseCode: " + rc +
                             (rc == "0" ? "  (SUCCESS)" : "  (FAILURE)"));
        } else if (respType == "GrantResponse") {
            Logger::ie_field("cbsdId:                " + json::get(resp, "cbsdId"));
            Logger::ie_field("grantId:               " + json::get(resp, "grantId") +
                             "  ← unique ID for this spectrum grant");
            Logger::ie_field("channelType:           " + json::get(resp, "channelType") +
                             "  ← GAA=free-to-use  PAL=licensed");
            Logger::ie_field("operationFrequencyMHz: " + json::get(resp, "operationFrequencyMHz") +
                             " MHz");
            Logger::ie_field("operationBandwidthMHz: " + json::get(resp, "operationBandwidthMHz") +
                             " MHz");
            Logger::ie_field("maxEirp:               " + json::get(resp, "maxEirp") +
                             " dBm  ← hard cap from SAS");
            Logger::ie_field("heartbeatInterval:     " + json::get(resp, "heartbeatInterval") +
                             " s  ← CBSD must heartbeat within this window");
            Logger::ie_field("grantExpireTime:       " + json::get(resp, "grantExpireTime") +
                             " s  ← entire grant expires after this");
            Logger::ie_field("responseCode:          " + rc +
                             (rc == "0" ? "  (SUCCESS)" : "  (FAILURE)"));
        } else if (respType == "HeartbeatResponse") {
            Logger::ie_field("cbsdId:             " + json::get(resp, "cbsdId"));
            Logger::ie_field("grantId:            " + json::get(resp, "grantId"));
            Logger::ie_field("transmitExpireTime: " + json::get(resp, "transmitExpireTime") +
                             " s  ← TX window renewed until this many seconds from now");
            Logger::ie_field("heartbeatInterval:  " + json::get(resp, "heartbeatInterval") +
                             " s  ← next heartbeat must arrive within this window");
            Logger::ie_field("responseCode:       " + rc);
        }

        // ── Update local state AFTER confirmed SAS response ───────────────
        std::string prevState = stateToString(info.state);

        if (respType == "RegistrationResponse" && success) {
            myCbsdId = json::get(resp, "cbsdId");

            // Now that we have a real cbsdId, rename this thread
            threadLabel = "cbsd-" + myCbsdId;
            Logger::setThreadName(threadLabel);

            CbsdInfo newInfo;
            newInfo.cbsdId    = myCbsdId;
            newInfo.fccId     = json::get(req, "fccId");
            newInfo.callSign  = json::get(req, "callSign");
            newInfo.category  = json::get(req, "cbsdCategory");
            newInfo.state     = SpectrumState::REGISTERED;
            g_registry.upsert(newInfo);

            Logger::sys("STATE: " + prevState + "  ──►  REGISTERED"
                        "  [cbsdId=" + myCbsdId + "]");
            Logger::sys("Registry now holds " + std::to_string(g_registry.count()) + " CBSD(s)");

        } else if (respType == "GrantResponse" && success) {
            std::string grantId = json::get(resp, "grantId");
            double freqMHz = 0, bwMHz = 0, maxEirp = 0;
            try { freqMHz = std::stod(json::get(resp, "operationFrequencyMHz")); } catch (...) {}
            try { bwMHz   = std::stod(json::get(resp, "operationBandwidthMHz")); } catch (...) {}
            try { maxEirp = std::stod(json::get(resp, "maxEirp")); } catch (...) {}
            g_registry.setGrant(myCbsdId, grantId, freqMHz, bwMHz, maxEirp);
            // setGrant() also sets state = GRANTED inside the registry

            Logger::sys("STATE: " + prevState + "  ──►  GRANTED"
                        "  [grantId=" + grantId +
                        "  freq=" + std::to_string(freqMHz) + " MHz"
                        "  bw=" + std::to_string(bwMHz) + " MHz]");
            Logger::sys("CBSD may now send HeartbeatRequest to start transmitting");

        } else if (respType == "HeartbeatResponse" && success) {
            g_registry.setState(myCbsdId, SpectrumState::AUTHORIZED);
            Logger::sys("STATE: " + prevState + "  ──►  AUTHORIZED"
                        "  [cbsdId=" + myCbsdId + " is now transmitting]");

        } else if (respType == "RelinquishmentResponse" && success) {
            g_registry.clearGrant(myCbsdId);
            // clearGrant() sets state back to REGISTERED
            Logger::sys("STATE: " + prevState + "  ──►  REGISTERED"
                        "  [grant released, spectrum returned to SAS pool]");

        } else if (respType == "DeregistrationResponse") {
            g_registry.remove(myCbsdId);
            Logger::sys("STATE: " + prevState + "  ──►  UNREGISTERED"
                        "  [" + myCbsdId + " removed from SAS and registry]");
            sendMsg(cbsdSock, resp);
            Logger::dp(Logger::Level::ENGINEER, "──► CBSD: DeregistrationResponse  (closing connection)");
            break;
        }

        // ── Forward SAS response back to CBSD ────────────────────────────
        sendMsg(cbsdSock, resp);
        Logger::dp(Logger::Level::ENGINEER, "──► CBSD: " + respType);
    }

    Logger::sys("Thread exiting — cbsdId=" +
                (myCbsdId.empty() ? "(never registered)" : myCbsdId));
}

// ── STATUS command ───────────────────────────────────────────────────────────
static void printStatus() {
    Logger::step("Domain Proxy STATUS");
    Logger::sys("Registry: " + std::to_string(g_registry.count()) + " CBSD(s) known");
    g_registry.forEach([](const CbsdInfo& c) {
        Logger::dp(Logger::Level::BEGINNER,
            "  cbsdId=" + c.cbsdId +
            "  fcc=" + c.fccId +
            "  cat=" + c.category +
            "  state=" + stateToString(c.state) +
            (c.grantId.empty() ? "" :
             "  grant=" + c.grantId +
             "  freq=" + std::to_string(c.grantFreqMHz) + " MHz"
             "  bw=" + std::to_string(c.grantBwMHz) + " MHz"));
    });
}

// ── main ─────────────────────────────────────────────────────────────────────
int main() {
    Logger::setThreadName("main");
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Logger::setSessionFile("dp_session.log");
    Logger::setLevelFromEnv();

    std::cout
        << "\n  +============================================+\n"
        << "  |  CBRS DOMAIN PROXY                         |\n"
        << "  |  Listens for CBSDs   on  port 8700         |\n"
        << "  |  Connects to SAS     on  port 8800         |\n"
        << "  |  Log level: set LOG_LEVEL=BEGINNER or ALL  |\n"
        << "  |  Commands:  STATUS   QUIT                  |\n"
        << "  +============================================+\n\n";

    Logger::sys("main thread starting — will start accept thread then run CLI");

    if (!connectToSas("127.0.0.1", 8800)) {
        std::cerr << "  Start sas_stub first!\n";
        return 1;
    }

    auto server = Socket::createServer("0.0.0.0", 8700);
    Logger::sys("Listening for CBSD connections on 0.0.0.0:8700");

    std::atomic<bool> stop{false};

    // ── Accept thread: waits for new CBSD connections and spawns a handler ──
    // This thread is named "accept" so you can see it in logs.
    std::thread acceptThread([&]() {
        Logger::setThreadName("accept");
        Logger::sys("accept thread started — blocking on accept() waiting for CBSDs");
        while (!stop.load()) {
            try {
                auto client = server.accept();
                int connId  = g_conn_counter.fetch_add(1);
                Logger::sys("New CBSD connected — spawning cbsd-conn" +
                            std::to_string(connId) + " thread to handle it");
                // Detach: this thread will run independently until the CBSD disconnects.
                // We don't join it because we don't know when it will finish.
                std::thread(handleCbsd, std::move(client), connId).detach();
                Logger::sys("cbsd-conn" + std::to_string(connId) +
                            " thread detached — accept thread back to blocking on accept()");
            } catch (...) {
                if (!stop.load()) Logger::warn("DP", "accept() threw — server socket closed?");
            }
        }
        Logger::sys("accept thread exiting");
    });
    acceptThread.detach();

    // ── CLI in main thread ────────────────────────────────────────────────
    Logger::sys("main thread now running CLI loop (STATUS / QUIT)");
    std::string line;
    std::cout << "dp> " << std::flush;
    while (!stop.load()) {
        if (!std::getline(std::cin, line)) {
            // stdin closed (e.g. redirected from /dev/null in scripted runs)
            // keep the proxy alive until SIGINT or stop is set
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        if (line == "STATUS" || line == "status") {
            printStatus();
        } else if (line == "QUIT" || line == "quit") {
            Logger::sys("QUIT — shutting down");
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
