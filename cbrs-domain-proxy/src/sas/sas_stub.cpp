// ============================================================
// SAS STUB — Simulated Spectrum Access System (port 8800)
//
// THREAD MAP:
//   [main thread]   — binds/listens on port 8800, named "main"
//   [dp-conn thread]— one per Domain Proxy connection, named "dp-conn"
//                     (a production SAS expects only one DP per zone)
//
// MESSAGE FLOW (this process only sees DP↔SAS, not CBSD↔DP):
//   DP sends a request for ONE CBSD at a time (serialised by g_sas_mtx in DP)
//   SAS processes it and sends a response
//   DP may then immediately send another CBSD's request
// ============================================================
#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include "common/logger.h"
#include "common/wire.h"

static std::atomic<int> g_cbsdCounter{1};
static std::atomic<int> g_grantCounter{1};

static std::string makeCbsdId()  { return "CBSD-SAS-" + std::to_string(g_cbsdCounter++); }
static std::string makeGrantId() { return "GRANT-"    + std::to_string(g_grantCounter++); }

// ── Handle one Domain Proxy connection ──────────────────────────────────────
static void handleDpConnection(Socket dpSock) {
    Logger::setThreadName("dp-conn");
    Logger::sys("Domain Proxy connected — dp-conn thread started");
    Logger::sys("Will handle all CBSD messages that DP forwards (one at a time)");

    while (true) {
        // ── Receive from Domain Proxy ─────────────────────────────────────
        std::string req;
        if (!recvMsg(dpSock, req)) {
            Logger::sys("Domain Proxy disconnected");
            break;
        }

        std::string type = json::get(req, "type");
        Logger::sas(Logger::Level::ENGINEER,
            "◄────────────────────────── DP: " + type);

        std::string resp;

        // ── Process each message type ─────────────────────────────────────
        if (type == "RegistrationRequest") {
            std::string fccId    = json::get(req, "fccId");
            std::string callSign = json::get(req, "callSign");
            std::string cat      = json::get(req, "cbsdCategory");
            std::string lat      = json::get(req, "latitude");
            std::string lon      = json::get(req, "longitude");
            std::string height   = json::get(req, "height");
            std::string newId    = makeCbsdId();

            Logger::sas(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("fccId:        " + fccId +
                             "  ← SAS checks this against FCC ELS database");
            Logger::ie_field("callSign:     " + callSign);
            Logger::ie_field("cbsdCategory: " + cat +
                             "  ← determines max EIRP (A=30dBm, B=47dBm)");
            Logger::ie_field("latitude:     " + lat +
                             "  ← must be accurate to 50 m (GPS required)");
            Logger::ie_field("longitude:    " + lon);
            Logger::ie_field("height:       " + height + " m AGL");

            Logger::sas(Logger::Level::ENGINEER,
                "Assigning cbsdId=" + newId + "  (CBSD-SAS-" +
                std::to_string(g_cbsdCounter-1) + " is the " +
                std::to_string(g_cbsdCounter-1) + "th CBSD registered this session)");

            resp = json::obj({
                {"type",     json::str("RegistrationResponse")},
                {"cbsdId",   json::str(newId)},
                {"response", json::obj({{"responseCode",    json::num(0)},
                                        {"responseMessage", json::str("SUCCESS")}})}
            });

            Logger::sas(Logger::Level::ENGINEER, "  Fields in response:");
            Logger::ie_field("cbsdId:       " + newId +
                             "  ← SAS-assigned ID, CBSD must include this in all future msgs");
            Logger::ie_field("responseCode: 0  (SUCCESS)");

        } else if (type == "GrantRequest") {
            std::string cbsdId  = json::get(req, "cbsdId");
            std::string freqStr = json::get(req, "operationFrequencyMHz");
            std::string bwStr   = json::get(req, "operationBandwidthMHz");
            std::string eirpStr = json::get(req, "maxEirp");
            double freq = freqStr.empty() ? 3555.0 : std::stod(freqStr);
            double bw   = bwStr.empty()   ? 10.0   : std::stod(bwStr);
            double eirp = eirpStr.empty()  ? 30.0   : std::stod(eirpStr);
            if (bw > 20.0) bw = 20.0;   // GAA cap per Part 96
            if (eirp > 30.0) eirp = 30.0;
            std::string grantId = makeGrantId();

            Logger::sas(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("cbsdId:               " + cbsdId);
            Logger::ie_field("operationFrequencyMHz:" + freqStr +
                             " MHz  ← CBSD's desired center frequency");
            Logger::ie_field("operationBandwidthMHz:" + bwStr +
                             " MHz  ← desired bandwidth (capped at 20 MHz for GAA)");
            Logger::ie_field("maxEirp:              " + eirpStr +
                             " dBm  ← capped at 30 dBm for Cat-A");

            Logger::sas(Logger::Level::ENGINEER,
                "Grant approved: GAA channel at " + std::to_string(freq) +
                " MHz  bw=" + std::to_string(bw) +
                " MHz  → assigning grantId=" + grantId);
            Logger::sas(Logger::Level::ENGINEER,
                "heartbeatInterval=120s — CBSD must heartbeat within 120s or grant is TERMINATED");

            resp = json::obj({
                {"type",                    json::str("GrantResponse")},
                {"cbsdId",                  json::str(cbsdId)},
                {"grantId",                 json::str(grantId)},
                {"channelType",             json::str("GAA")},
                {"operationFrequencyMHz",   json::flt(freq)},
                {"operationBandwidthMHz",   json::flt(bw)},
                {"maxEirp",                 json::num((int)eirp)},
                {"grantExpireTime",         json::num(3600)},
                {"heartbeatInterval",       json::num(120)},
                {"response",                json::obj({{"responseCode", json::num(0)}})}
            });

            Logger::sas(Logger::Level::ENGINEER, "  Fields in response:");
            Logger::ie_field("grantId:             " + grantId);
            Logger::ie_field("channelType:         GAA  ← General Authorized Access (free)");
            Logger::ie_field("operationFrequencyMHz:" + std::to_string(freq) + " MHz");
            Logger::ie_field("operationBandwidthMHz:" + std::to_string(bw) + " MHz");
            Logger::ie_field("maxEirp:             " + std::to_string((int)eirp) + " dBm");
            Logger::ie_field("heartbeatInterval:   120 s");
            Logger::ie_field("grantExpireTime:     3600 s  (1 hour)");
            Logger::ie_field("responseCode:        0  (SUCCESS)");

        } else if (type == "HeartbeatRequest") {
            std::string cbsdId  = json::get(req, "cbsdId");
            std::string grantId = json::get(req, "grantId");
            std::string opState = json::get(req, "operationState");

            Logger::sas(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("cbsdId:         " + cbsdId);
            Logger::ie_field("grantId:        " + grantId);
            Logger::ie_field("operationState: " + opState +
                             "  ← GRANTED=ready-but-not-TX, AUTHORIZED=actively-TX");

            Logger::sas(Logger::Level::ENGINEER,
                "Heartbeat OK — renewing transmit window for " + cbsdId);
            Logger::sas(Logger::Level::ENGINEER,
                "transmitExpireTime=3600s — CBSD may transmit until then (or next heartbeat)");

            resp = json::obj({
                {"type",               json::str("HeartbeatResponse")},
                {"cbsdId",             json::str(cbsdId)},
                {"grantId",            json::str(grantId)},
                {"transmitExpireTime", json::num(3600)},
                {"heartbeatInterval",  json::num(120)},
                {"response",           json::obj({{"responseCode", json::num(0)}})}
            });

            Logger::sas(Logger::Level::ENGINEER, "  Fields in response:");
            Logger::ie_field("transmitExpireTime: 3600 s  ← renewed TX window");
            Logger::ie_field("heartbeatInterval:  120 s   ← must send next HB within this");
            Logger::ie_field("responseCode:       0  (SUCCESS)");

        } else if (type == "RelinquishmentRequest") {
            std::string cbsdId  = json::get(req, "cbsdId");
            std::string grantId = json::get(req, "grantId");

            Logger::sas(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("cbsdId:  " + cbsdId);
            Logger::ie_field("grantId: " + grantId + "  ← being returned to spectrum pool");

            Logger::sas(Logger::Level::ENGINEER,
                "Grant " + grantId + " relinquished — channel freed for other CBSDs");

            resp = json::obj({
                {"type",     json::str("RelinquishmentResponse")},
                {"cbsdId",   json::str(cbsdId)},
                {"grantId",  json::str(grantId)},
                {"response", json::obj({{"responseCode", json::num(0)}})}
            });

        } else if (type == "DeregistrationRequest") {
            std::string cbsdId = json::get(req, "cbsdId");

            Logger::sas(Logger::Level::ENGINEER, "  Fields received:");
            Logger::ie_field("cbsdId: " + cbsdId + "  ← being removed from SAS database");

            Logger::sas(Logger::Level::ENGINEER,
                cbsdId + " removed — future messages with this cbsdId will fail");

            resp = json::obj({
                {"type",     json::str("DeregistrationResponse")},
                {"cbsdId",   json::str(cbsdId)},
                {"response", json::obj({{"responseCode", json::num(0)}})}
            });

        } else {
            Logger::warn("SAS", "Unknown message type: " + type);
            resp = json::obj({
                {"type",     json::str("ErrorResponse")},
                {"response", json::obj({{"responseCode",    json::num(103)},
                                        {"responseMessage", json::str("INVALID_VALUE")}})}
            });
        }

        // ── Send response back to DP ──────────────────────────────────────
        std::string respType = json::get(resp, "type");
        Logger::sas(Logger::Level::ENGINEER,
            "──────────────────────────► DP: " + respType);
        sendMsg(dpSock, resp);
    }

    Logger::sys("dp-conn thread exiting");
}

int main() {
    Logger::setThreadName("main");
    Logger::setSessionFile("sas_session.log");
    Logger::setLevelFromEnv();

    std::cout
        << "\n  +============================================+\n"
        << "  |  CBRS SAS STUB                             |\n"
        << "  |  Listens for Domain Proxy on port 8800     |\n"
        << "  |  Simulates a Spectrum Access System        |\n"
        << "  +============================================+\n\n";

    Logger::sys("main thread — binding to port 8800, waiting for Domain Proxy");

    try {
        auto server = Socket::createServer("0.0.0.0", 8800);
        Logger::sys("SAS listening on 0.0.0.0:8800");
        Logger::sys("Start domain_proxy next — it will connect here");

        while (true) {
            auto client = server.accept();
            Logger::sys("Domain Proxy connected — spawning dp-conn thread");
            std::thread(handleDpConnection, std::move(client)).detach();
        }
    } catch (const std::exception& ex) {
        Logger::warn("SAS", ex.what());
        return 1;
    }
    return 0;
}
