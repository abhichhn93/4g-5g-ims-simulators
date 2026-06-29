// ============================================================
// CBSD AGENT — Simulated base station device
//
// THREAD MAP:
//   [main thread]  — CLI + sends/receives messages, named "main"
//   No other threads — CBSD is simple: one command at a time.
//
// FULL MESSAGE FLOW (what you see across all 3 terminals):
//
//   cbsd_agent          domain_proxy            sas_stub
//   [main]              [cbsd-conn0]            [dp-conn]
//      │                     │                      │
//      │── REGISTER ────────►│                      │
//      │   (TCP, port 8700)  │── RegistrationReq ──►│
//      │                     │                      │ assigns cbsdId
//      │                     │◄── RegistrationResp ─│
//      │◄── RegistrationResp ─│ (state: REGISTERED) │
//      │                     │                      │
//      │── GRANT ────────────►│                      │
//      │                     │── GrantRequest ──────►│
//      │                     │                      │ assigns grantId + channel
//      │                     │◄── GrantResponse ────│
//      │◄── GrantResponse ───│ (state: GRANTED)     │
//      │                     │                      │
//      │── HEARTBEAT ─────────►│                      │
//      │                     │── HeartbeatRequest ──►│
//      │                     │◄── HeartbeatResponse ─│
//      │◄── HeartbeatResponse │ (state: AUTHORIZED)  │
// ============================================================
#include <iostream>
#include <string>
#include <map>
#include <atomic>
#include <csignal>
#include "common/logger.h"
#include "common/wire.h"
#include "proxy/spectrum_state.h"

static std::atomic<bool> g_stop{false};
static void sig_handler(int) { g_stop.store(true); }

struct CbsdConfig {
    std::string id, fccId, callSign, category;
    double latitude, longitude, heightMeters, antennaGainDbi;
    double desiredFreqMHz, desiredBwMHz;
};

static std::map<std::string, CbsdConfig> CBSD_PROFILES = {
    {"1", {"CBSD-1", "FCC-DEVICE-001", "KA1ABC", "A",
           37.4219, -122.0841, 5.0, 6.0, 3555.0, 10.0}},
    {"2", {"CBSD-2", "FCC-DEVICE-002", "KA2DEF", "A",
           37.4220, -122.0845, 8.0, 6.0, 3565.0, 10.0}},
    {"3", {"CBSD-3", "FCC-DEVICE-003", "KB3GHI", "B",
           37.4225, -122.0850, 30.0, 8.0, 3580.0, 20.0}},
};

int main(int argc, char** argv) {
    Logger::setThreadName("main");
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    Logger::setSessionFile("cbsd_session.log");
    Logger::setLevelFromEnv();
    std::signal(SIGINT, sig_handler);

    std::string profileId = (argc > 1) ? argv[1] : "1";
    if (CBSD_PROFILES.find(profileId) == CBSD_PROFILES.end()) {
        std::cerr << "Usage: ./cbsd_agent [1|2|3]\n"
                  << "  1 = Cat-A indoor, 3555 MHz, 10 MHz BW\n"
                  << "  2 = Cat-A indoor, 3565 MHz, 10 MHz BW\n"
                  << "  3 = Cat-B outdoor, 3580 MHz, 20 MHz BW\n";
        return 1;
    }
    const CbsdConfig& cfg = CBSD_PROFILES.at(profileId);

    std::cout
        << "\n  +============================================+\n"
        << "  |  CBSD Agent " << cfg.id << " — Category " << cfg.category << "\n"
        << "  |  FCC ID:     " << cfg.fccId << "\n"
        << "  |  Freq:       " << cfg.desiredFreqMHz << " MHz  BW:" << cfg.desiredBwMHz << " MHz\n"
        << "  |  Thread:     main (single-threaded — one command at a time)\n"
        << "  +============================================+\n"
        << "  Commands: REGISTER  GRANT  HEARTBEAT  RELINQUISH  DEREGISTER  STATUS  QUIT\n\n";

    Logger::sys("main thread — connecting to Domain Proxy on 127.0.0.1:8700 ...");

    Socket conn;
    try {
        conn = Socket::connectTo("127.0.0.1", 8700);
    } catch (const std::exception& ex) {
        std::cerr << "Cannot connect to Domain Proxy on port 8700. Start it first!\n";
        return 1;
    }
    Logger::sys("Connected to Domain Proxy ✓");
    Logger::sys("All messages go: cbsd_agent ──TCP:8700──► domain_proxy ──TCP:8800──► sas_stub");

    SpectrumState state = SpectrumState::UNREGISTERED;
    std::string cbsdId, grantId;
    double grantFreq = 0, grantBw = 0, maxEirp = 0;

    auto printState = [&]() {
        std::cout
            << "\n  ┌── CBSD STATE ──────────────────────────────────\n"
            << "  │  thread:  main  (single-threaded agent)\n"
            << "  │  cbsdId:  " << (cbsdId.empty() ? "(not yet assigned by SAS)" : cbsdId) << "\n"
            << "  │  state:   " << stateToString(state) << "\n";
        if (!grantId.empty())
            std::cout
                << "  │  grantId: " << grantId << "\n"
                << "  │  channel: " << grantFreq << " MHz  BW:" << grantBw
                << " MHz  maxEirp:" << maxEirp << " dBm\n";
        std::cout << "  └───────────────────────────────────────────────\n\n";
    };

    std::string line;
    std::cout << cfg.id << "> " << std::flush;

    while (!g_stop.load() && std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << cfg.id << "> " << std::flush; continue; }

        std::string resp;

        // ── REGISTER ─────────────────────────────────────────────────────
        if (line == "REGISTER" || line == "register") {
            if (state != SpectrumState::UNREGISTERED) {
                std::cout << "  Already registered (cbsdId=" << cbsdId << ").\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            Logger::cbsd(Logger::Level::ENGINEER,
                "main thread sending RegistrationRequest to Domain Proxy");
            Logger::cbsd(Logger::Level::ENGINEER, "  Fields being sent:");
            Logger::ie_field("fccId:          " + cfg.fccId);
            Logger::ie_field("callSign:       " + cfg.callSign);
            Logger::ie_field("cbsdCategory:   " + cfg.category);
            Logger::ie_field("radioTechnology:NR");
            Logger::ie_field("latitude:       " + std::to_string(cfg.latitude));
            Logger::ie_field("longitude:      " + std::to_string(cfg.longitude));
            Logger::ie_field("height:         " + std::to_string(cfg.heightMeters) + " m AGL");
            Logger::ie_field("antennaGain:    " + std::to_string(cfg.antennaGainDbi) + " dBi");

            Logger::cbsd(Logger::Level::ENGINEER,
                "──────────────────────────► DP: RegistrationRequest  (blocking send+recv)");

            std::string req = json::obj({
                {"type",            json::str("RegistrationRequest")},
                {"fccId",           json::str(cfg.fccId)},
                {"callSign",        json::str(cfg.callSign)},
                {"cbsdCategory",    json::str(cfg.category)},
                {"radioTechnology", json::str("NR")},
                {"latitude",        json::flt(cfg.latitude)},
                {"longitude",       json::flt(cfg.longitude)},
                {"height",          json::flt(cfg.heightMeters)},
                {"heightType",      json::str("AGL")},
                {"antennaGain",     json::flt(cfg.antennaGainDbi)},
                {"measCapability",  json::str("RECEIVED_POWER_WITH_GRANT")},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;

            Logger::cbsd(Logger::Level::ENGINEER,
                "◄────────────────────────── DP: RegistrationResponse  received");

            cbsdId = json::get(resp, "cbsdId");
            std::string rc = json::get(resp, "responseCode");

            Logger::cbsd(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("cbsdId:       " + cbsdId +
                             "  ← SAS assigned this; include in ALL future messages");
            Logger::ie_field("responseCode: " + rc +
                             (rc == "0" ? "  (SUCCESS)" : "  (FAILURE)"));

            if (rc == "0" || !cbsdId.empty()) {
                std::string prev = stateToString(state);
                state = SpectrumState::REGISTERED;
                Logger::sys("STATE: " + prev + "  ──►  REGISTERED  [cbsdId=" + cbsdId + "]");
                std::cout << "\n  ✓ REGISTERED  cbsdId=" << cbsdId
                          << "\n  Next: type GRANT to request spectrum\n\n";
            } else {
                Logger::warn("CBSD", "Registration failed: " + resp);
            }

        // ── GRANT ─────────────────────────────────────────────────────────
        } else if (line == "GRANT" || line == "grant") {
            if (state != SpectrumState::REGISTERED) {
                std::cout << "  Must REGISTER first. Current: " << stateToString(state) << "\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            Logger::cbsd(Logger::Level::ENGINEER,
                "main thread sending GrantRequest to Domain Proxy");
            Logger::cbsd(Logger::Level::ENGINEER, "  Fields being sent:");
            Logger::ie_field("cbsdId:               " + cbsdId);
            Logger::ie_field("operationFrequencyMHz:" + std::to_string(cfg.desiredFreqMHz) +
                             " MHz");
            Logger::ie_field("operationBandwidthMHz:" + std::to_string(cfg.desiredBwMHz) +
                             " MHz");
            Logger::ie_field("maxEirp:              " +
                             std::to_string(cfg.category == "A" ? 30 : 47) +
                             " dBm  ← Cat-A cap=30, Cat-B cap=47");

            Logger::cbsd(Logger::Level::ENGINEER,
                "──────────────────────────► DP: GrantRequest");

            std::string req = json::obj({
                {"type",                  json::str("GrantRequest")},
                {"cbsdId",                json::str(cbsdId)},
                {"operationFrequencyMHz", json::flt(cfg.desiredFreqMHz)},
                {"operationBandwidthMHz", json::flt(cfg.desiredBwMHz)},
                {"maxEirp",               json::num(cfg.category == "A" ? 30 : 47)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;

            Logger::cbsd(Logger::Level::ENGINEER,
                "◄────────────────────────── DP: GrantResponse  received");

            grantId = json::get(resp, "grantId");
            try { grantFreq = std::stod(json::get(resp, "operationFrequencyMHz")); } catch (...) {}
            try { grantBw   = std::stod(json::get(resp, "operationBandwidthMHz")); } catch (...) {}
            try { maxEirp   = std::stod(json::get(resp, "maxEirp")); } catch (...) {}
            std::string rc = json::get(resp, "responseCode");

            Logger::cbsd(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("grantId:             " + grantId);
            Logger::ie_field("channelType:         " + json::get(resp, "channelType") +
                             "  ← GAA=free  PAL=licensed");
            Logger::ie_field("operationFrequencyMHz:" + std::to_string(grantFreq) + " MHz");
            Logger::ie_field("operationBandwidthMHz:" + std::to_string(grantBw) + " MHz");
            Logger::ie_field("maxEirp:             " + std::to_string(maxEirp) + " dBm");
            Logger::ie_field("heartbeatInterval:   " + json::get(resp, "heartbeatInterval") +
                             " s  ← send HEARTBEAT within this window or grant is TERMINATED");
            Logger::ie_field("responseCode:        " + rc);

            if (rc == "0" || !grantId.empty()) {
                std::string prev = stateToString(state);
                state = SpectrumState::GRANTED;
                Logger::sys("STATE: " + prev + "  ──►  GRANTED  [grantId=" + grantId + "]");
                std::cout << "\n  ✓ GRANTED  grantId=" << grantId
                          << "  freq=" << grantFreq << " MHz  bw=" << grantBw << " MHz"
                          << "\n  Next: type HEARTBEAT to start transmitting\n\n";
            }

        // ── HEARTBEAT ─────────────────────────────────────────────────────
        } else if (line == "HEARTBEAT" || line == "heartbeat") {
            if (state != SpectrumState::GRANTED && state != SpectrumState::AUTHORIZED) {
                std::cout << "  Need an active grant. Type GRANT first.\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            std::string opState = (state == SpectrumState::AUTHORIZED) ? "AUTHORIZED" : "GRANTED";
            Logger::cbsd(Logger::Level::ENGINEER,
                "main thread sending HeartbeatRequest  (operationState=" + opState + ")");
            Logger::cbsd(Logger::Level::ENGINEER, "  Fields being sent:");
            Logger::ie_field("cbsdId:         " + cbsdId);
            Logger::ie_field("grantId:        " + grantId);
            Logger::ie_field("operationState: " + opState +
                             "  ← tells SAS if we're ready-to-TX or already-TX");

            Logger::cbsd(Logger::Level::ENGINEER,
                "──────────────────────────► DP: HeartbeatRequest");

            std::string req = json::obj({
                {"type",           json::str("HeartbeatRequest")},
                {"cbsdId",         json::str(cbsdId)},
                {"grantId",        json::str(grantId)},
                {"operationState", json::str(opState)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;

            Logger::cbsd(Logger::Level::ENGINEER,
                "◄────────────────────────── DP: HeartbeatResponse  received");

            std::string txExpire = json::get(resp, "transmitExpireTime");
            std::string hbInt    = json::get(resp, "heartbeatInterval");

            Logger::cbsd(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("transmitExpireTime: " + txExpire +
                             " s  ← TX window renewed; must heartbeat again before this expires");
            Logger::ie_field("heartbeatInterval:  " + hbInt + " s");
            Logger::ie_field("responseCode:       " + json::get(resp, "responseCode"));

            std::string prev = stateToString(state);
            state = SpectrumState::AUTHORIZED;
            Logger::sys("STATE: " + prev + "  ──►  AUTHORIZED  [CBSD is now transmitting]");
            std::cout << "\n  ✓ AUTHORIZED — transmitting on " << grantFreq << " MHz\n\n";

        // ── RELINQUISH ────────────────────────────────────────────────────
        } else if (line == "RELINQUISH" || line == "relinquish") {
            if (grantId.empty()) {
                std::cout << "  No active grant.\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            Logger::cbsd(Logger::Level::ENGINEER,
                "Sending RelinquishmentRequest — voluntarily returning spectrum");
            Logger::cbsd(Logger::Level::ENGINEER, "  Fields being sent:");
            Logger::ie_field("cbsdId:  " + cbsdId);
            Logger::ie_field("grantId: " + grantId + "  ← the grant being returned");

            Logger::cbsd(Logger::Level::ENGINEER,
                "──────────────────────────► DP: RelinquishmentRequest");

            std::string req = json::obj({
                {"type",    json::str("RelinquishmentRequest")},
                {"cbsdId",  json::str(cbsdId)},
                {"grantId", json::str(grantId)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;

            Logger::cbsd(Logger::Level::ENGINEER,
                "◄────────────────────────── DP: RelinquishmentResponse");

            std::string prev = stateToString(state);
            grantId.clear(); grantFreq = 0; grantBw = 0; maxEirp = 0;
            state = SpectrumState::REGISTERED;
            Logger::sys("STATE: " + prev + "  ──►  REGISTERED  [grant cleared locally]");
            std::cout << "  ✓ Grant relinquished — spectrum returned to SAS pool\n\n";

        // ── DEREGISTER ────────────────────────────────────────────────────
        } else if (line == "DEREGISTER" || line == "deregister") {
            if (cbsdId.empty()) {
                std::cout << "  Not registered.\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            Logger::cbsd(Logger::Level::ENGINEER,
                "Sending DeregistrationRequest — removing CBSD from SAS");
            Logger::cbsd(Logger::Level::ENGINEER, "  Fields being sent:");
            Logger::ie_field("cbsdId: " + cbsdId);

            Logger::cbsd(Logger::Level::ENGINEER,
                "──────────────────────────► DP: DeregistrationRequest");

            std::string req = json::obj({
                {"type",   json::str("DeregistrationRequest")},
                {"cbsdId", json::str(cbsdId)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;

            Logger::cbsd(Logger::Level::ENGINEER,
                "◄────────────────────────── DP: DeregistrationResponse");

            std::string prev = stateToString(state);
            cbsdId.clear(); grantId.clear();
            state = SpectrumState::UNREGISTERED;
            Logger::sys("STATE: " + prev + "  ──►  UNREGISTERED");
            std::cout << "  ✓ Deregistered\n\n";

        } else if (line == "STATUS" || line == "status") {
            printState();
        } else if (line == "QUIT" || line == "quit") {
            break;
        } else {
            std::cout << "  Commands: REGISTER  GRANT  HEARTBEAT  RELINQUISH  DEREGISTER  STATUS  QUIT\n";
        }

        std::cout << cfg.id << "> " << std::flush;
    }

    Logger::sys("main thread exiting");
    Logger::shutdown();
    return 0;
}
