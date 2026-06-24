// ============================================================
// CBSD AGENT — Simulated Citizens Broadband Radio Service Device
//
// A CBSD is the "smart" base station in a CBRS deployment:
//   Category A: indoor/low-power (≤30 dBm EIRP, e.g. office small cell)
//   Category B: outdoor/high-power (≤47 dBm EIRP, e.g. campus macro cell)
//
// This agent simulates the CBSD firmware's SAS client:
//   1. Connects to the Domain Proxy on port 8700
//   2. Runs the full WinnForum SAS-CBSD protocol (WINNF-TS-0016)
//   3. CLI lets you trigger each step manually — great for demos
//
// Commands:
//   REGISTER   — send RegistrationRequest (FCC ID, location, antenna info)
//   GRANT      — send GrantRequest (desired frequency + bandwidth)
//   HEARTBEAT  — send HeartbeatRequest (keep transmit window alive)
//   RELINQUISH — voluntarily return the spectrum grant
//   DEREGISTER — remove CBSD from SAS database
//   STATUS     — show current state machine state
//   QUIT       — disconnect
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

// ── CBSD configuration (command-line configurable) ──────────────────────────
struct CbsdConfig {
    std::string id;            // "CBSD-1", "CBSD-2", etc.
    std::string fccId;         // FCC equipment authorization ID
    std::string callSign;      // operator call sign
    std::string category;      // "A" or "B"
    double      latitude;
    double      longitude;
    double      heightMeters;
    double      antennaGainDbi;
    double      desiredFreqMHz;   // center freq the CBSD wants
    double      desiredBwMHz;     // desired bandwidth
};

// ── Predefined CBSD profiles ─────────────────────────────────────────────────
static std::map<std::string, CbsdConfig> CBSD_PROFILES = {
    {"1", {"CBSD-1", "FCC-DEVICE-001", "KA1ABC", "A",
           37.4219, -122.0841, 5.0, 6.0, 3555.0, 10.0}},
    {"2", {"CBSD-2", "FCC-DEVICE-002", "KA2DEF", "A",
           37.4220, -122.0845, 8.0, 6.0, 3565.0, 10.0}},
    {"3", {"CBSD-3", "FCC-DEVICE-003", "KB3GHI", "B",
           37.4225, -122.0850, 30.0, 8.0, 3580.0, 20.0}},
};

int main(int argc, char** argv) {
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

    std::cout << "\n"
              << "  +==========================================+\n"
              << "  |  CBSD Agent " << cfg.id << " — Category " << cfg.category << "\n"
              << "  |  FCC ID:     " << cfg.fccId << "\n"
              << "  |  Call Sign:  " << cfg.callSign << "\n"
              << "  |  Location:   " << cfg.latitude << "," << cfg.longitude << "\n"
              << "  |  Freq:       " << cfg.desiredFreqMHz << " MHz  BW:" << cfg.desiredBwMHz << " MHz\n"
              << "  +==========================================+\n"
              << "  Commands: REGISTER  GRANT  HEARTBEAT  RELINQUISH  DEREGISTER  STATUS  QUIT\n"
              << "  ----------------------------------------\n\n" << std::flush;

    Logger::cbsd(Logger::Level::INTERVIEW_T,
        "[CBRS SPEC] CBSD = Citizens Broadband Radio Service Device. "
        "Category A: max 30 dBm EIRP, indoor/low-power (small cells). "
        "Category B: max 47 dBm EIRP, outdoor (macro cells). "
        "FCC ID is the equipment authorization ID from the FCC OET ELS database. "
        "All CBSDs must be certified for CBRS and GPS-capable (location required).");

    // Connect to Domain Proxy
    Socket conn;
    try {
        conn = Socket::connectTo("127.0.0.1", 8700);
    } catch (const std::exception& ex) {
        std::cerr << "  Cannot connect to Domain Proxy on port 8700. "
                  << "Start domain_proxy first!\n";
        return 1;
    }
    std::cout << "  Connected to Domain Proxy ✓\n\n" << std::flush;

    // State tracking
    SpectrumState state = SpectrumState::UNREGISTERED;
    std::string cbsdId, grantId;
    double grantFreq = 0, grantBw = 0, maxEirp = 0;

    auto printState = [&]() {
        std::cout << "\n  ┌── CBSD STATUS ─────────────────────────────\n"
                  << "  │  cbsdId:  " << (cbsdId.empty() ? "(not assigned)" : cbsdId) << "\n"
                  << "  │  state:   " << stateToString(state) << "\n";
        if (!grantId.empty())
            std::cout << "  │  grantId: " << grantId << "\n"
                      << "  │  channel: " << grantFreq << " MHz  BW:" << grantBw
                      << " MHz  maxEirp:" << maxEirp << " dBm\n";
        std::cout << "  └─────────────────────────────────────────────\n\n";
    };

    std::string line;
    std::cout << cfg.id << "> " << std::flush;

    while (!g_stop.load() && std::getline(std::cin, line)) {
        if (line.empty()) { std::cout << cfg.id << "> " << std::flush; continue; }

        std::string resp;

        if (line == "REGISTER" || line == "register") {
            // ── Step 1: Registration ────────────────────────────────────────
            if (state != SpectrumState::UNREGISTERED) {
                std::cout << "  Already registered (cbsdId=" << cbsdId << ")\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            std::cout << "  → RegistrationRequest\n" << std::flush;
            Logger::cbsd(Logger::Level::BEGINNER,
                "Sending RegistrationRequest: FCC ID=" + cfg.fccId +
                " category=" + cfg.category + " location=(" +
                std::to_string(cfg.latitude) + "," + std::to_string(cfg.longitude) + ")");
            Logger::ie_field("  fccId:        " + cfg.fccId + "  ← FCC equipment auth ID");
            Logger::ie_field("  callSign:     " + cfg.callSign + "  ← operator call sign");
            Logger::ie_field("  cbsdCategory: " + cfg.category + "  ← A(≤30dBm) or B(≤47dBm)");
            Logger::ie_field("  latitude:     " + std::to_string(cfg.latitude));
            Logger::ie_field("  longitude:    " + std::to_string(cfg.longitude));
            Logger::ie_field("  height:       " + std::to_string(cfg.heightMeters) + " m AGL");
            Logger::ie_field("  antennaGain:  " + std::to_string(cfg.antennaGainDbi) + " dBi");

            std::string req = json::obj({
                {"type",           json::str("RegistrationRequest")},
                {"fccId",          json::str(cfg.fccId)},
                {"callSign",       json::str(cfg.callSign)},
                {"cbsdCategory",   json::str(cfg.category)},
                {"radioTechnology",json::str("NR")},
                {"latitude",       json::flt(cfg.latitude)},
                {"longitude",      json::flt(cfg.longitude)},
                {"height",         json::flt(cfg.heightMeters)},
                {"heightType",     json::str("AGL")},
                {"antennaGain",    json::flt(cfg.antennaGainDbi)},
                {"measCapability", json::str("RECEIVED_POWER_WITH_GRANT")},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;

            cbsdId = json::get(resp, "cbsdId");
            std::string rc = json::get(resp, "responseCode");
            if (rc == "0" || cbsdId.size() > 0) {
                state = SpectrumState::REGISTERED;
                std::cout << "\n  ╔══════════════════════════════════════╗\n"
                          << "  ║  ✓ REGISTERED  cbsdId=" << cbsdId << "\n"
                          << "  ╚══════════════════════════════════════╝\n\n" << std::flush;
                Logger::cbsd(Logger::Level::BEGINNER,
                    "Registration complete. Can now request spectrum (type GRANT).");
            } else {
                std::cout << "  ✗ Registration failed: " << resp << "\n";
            }

        } else if (line == "GRANT" || line == "grant") {
            // ── Step 2: Grant Request ────────────────────────────────────────
            if (state != SpectrumState::REGISTERED) {
                std::cout << "  Must be REGISTERED first. Current: " << stateToString(state) << "\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            std::cout << "  → GrantRequest  freq=" << cfg.desiredFreqMHz
                      << " MHz  BW=" << cfg.desiredBwMHz << " MHz\n" << std::flush;

            Logger::cbsd(Logger::Level::BEGINNER,
                "Requesting spectrum: " + std::to_string(cfg.desiredFreqMHz) +
                " MHz center, " + std::to_string(cfg.desiredBwMHz) + " MHz bandwidth");
            Logger::cbsd(Logger::Level::INTERVIEW_T,
                "[CBRS SPEC] CBRS operates in 3550-3700 MHz (150 MHz). "
                "PAL channels: 3550-3620 MHz (7×10 MHz blocks, licensed). "
                "GAA channels: entire band, opportunistic. "
                "SAS picks the best available channel based on interference coordination.");
            Logger::ie_field("  operationFrequencyMHz: " + std::to_string(cfg.desiredFreqMHz));
            Logger::ie_field("  operationBandwidthMHz: " + std::to_string(cfg.desiredBwMHz));
            Logger::ie_field("  maxEirp: 30 dBm (Cat-A) — SAS will enforce this cap");

            std::string req = json::obj({
                {"type",                    json::str("GrantRequest")},
                {"cbsdId",                  json::str(cbsdId)},
                {"operationFrequencyMHz",   json::flt(cfg.desiredFreqMHz)},
                {"operationBandwidthMHz",   json::flt(cfg.desiredBwMHz)},
                {"maxEirp",                 json::num(cfg.category == "A" ? 30 : 47)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;

            grantId = json::get(resp, "grantId");
            try { grantFreq = std::stod(json::get(resp, "operationFrequencyMHz")); } catch (...) {}
            try { grantBw   = std::stod(json::get(resp, "operationBandwidthMHz")); } catch (...) {}
            try { maxEirp   = std::stod(json::get(resp, "maxEirp")); } catch (...) {}
            std::string rc = json::get(resp, "responseCode");
            if (rc == "0" || !grantId.empty()) {
                state = SpectrumState::GRANTED;
                std::cout << "\n  ╔══════════════════════════════════════╗\n"
                          << "  ║  ✓ GRANTED  grantId=" << grantId << "\n"
                          << "  ║  freq=" << grantFreq << " MHz  BW=" << grantBw
                          << " MHz  maxEirp=" << maxEirp << " dBm\n"
                          << "  ╚══════════════════════════════════════╝\n"
                          << "  → Type HEARTBEAT to start transmitting\n\n" << std::flush;
            } else {
                std::cout << "  ✗ Grant failed: " << resp << "\n";
            }

        } else if (line == "HEARTBEAT" || line == "heartbeat") {
            // ── Step 3: Heartbeat ─────────────────────────────────────────────
            if (state != SpectrumState::GRANTED && state != SpectrumState::AUTHORIZED) {
                std::cout << "  Need an active grant first. Type GRANT.\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            std::string opState = (state == SpectrumState::AUTHORIZED) ? "AUTHORIZED" : "GRANTED";
            std::cout << "  → HeartbeatRequest  operationState=" << opState << "\n" << std::flush;
            Logger::cbsd(Logger::Level::BEGINNER,
                "Heartbeat sent — refreshes transmit window. Miss it and SAS revokes grant!");
            Logger::cbsd(Logger::Level::INTERVIEW_T,
                "[CBRS SPEC] Heartbeat proves the CBSD is still alive and at its registered "
                "location. heartbeatInterval = 120s (SAS-controlled). Missing a heartbeat: "
                "SAS sends TERMINATED_GRANT and CBSD must stop TX within 60s (Move List timer). "
                "ESC sensors can also force immediate heartbeat in emergency.");

            std::string req = json::obj({
                {"type",           json::str("HeartbeatRequest")},
                {"cbsdId",         json::str(cbsdId)},
                {"grantId",        json::str(grantId)},
                {"operationState", json::str(opState)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;
            state = SpectrumState::AUTHORIZED;
            std::cout << "\n  ╔══════════════════════════════════════╗\n"
                      << "  ║  ✓ AUTHORIZED — CBSD is transmitting\n"
                      << "  ║  on " << grantFreq << " MHz  (NR 5G private network)\n"
                      << "  ╚══════════════════════════════════════╝\n\n" << std::flush;

        } else if (line == "RELINQUISH" || line == "relinquish") {
            // ── Step 4: Relinquishment ────────────────────────────────────────
            if (grantId.empty()) {
                std::cout << "  No active grant to relinquish.\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            std::cout << "  → RelinquishmentRequest  grantId=" << grantId << "\n" << std::flush;
            Logger::cbsd(Logger::Level::BEGINNER,
                "Voluntarily returning spectrum grant — frees channel for other CBSDs");

            std::string req = json::obj({
                {"type",    json::str("RelinquishmentRequest")},
                {"cbsdId",  json::str(cbsdId)},
                {"grantId", json::str(grantId)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;
            grantId.clear(); grantFreq = 0; grantBw = 0; maxEirp = 0;
            state = SpectrumState::REGISTERED;
            std::cout << "  ✓ Grant relinquished — state=REGISTERED\n\n" << std::flush;

        } else if (line == "DEREGISTER" || line == "deregister") {
            // ── Step 5: Deregistration ────────────────────────────────────────
            if (cbsdId.empty()) {
                std::cout << "  Not registered.\n";
                std::cout << cfg.id << "> " << std::flush; continue;
            }
            std::cout << "  → DeregistrationRequest  cbsdId=" << cbsdId << "\n" << std::flush;
            std::string req = json::obj({
                {"type",   json::str("DeregistrationRequest")},
                {"cbsdId", json::str(cbsdId)},
            });
            sendMsg(conn, req);
            if (!recvMsg(conn, resp)) break;
            cbsdId.clear(); grantId.clear();
            state = SpectrumState::UNREGISTERED;
            std::cout << "  ✓ Deregistered — state=UNREGISTERED\n\n" << std::flush;

        } else if (line == "STATUS" || line == "status") {
            printState();

        } else if (line == "QUIT" || line == "quit") {
            break;
        } else {
            std::cout << "  Commands: REGISTER  GRANT  HEARTBEAT  RELINQUISH  DEREGISTER  STATUS  QUIT\n";
        }

        std::cout << cfg.id << "> " << std::flush;
    }

    Logger::shutdown();
    return 0;
}
