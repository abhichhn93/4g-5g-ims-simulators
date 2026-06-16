// e2_agent.cpp — O-RAN E2 Agent running on the eNB side.
// Accepts connections from xApps, handles E2SetupRequest and
// RICControlRequest, logs with BEGINNER/ENGINEER/INTERVIEW narration.
// Port 36421 (E2 interface, TS 38.473 / O-RAN E2AP).
// Build: see CMakeLists.txt target e2_agent
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <thread>
#include <chrono>
#include "common/logger.h"
#include "common/json_event_log.h"

static constexpr uint16_t E2_PORT = 36421;

// ── Minimal JSON helpers (no external lib) ──────────────────────────────────
namespace je2 {
    inline std::string str(const std::string& s) { return "\"" + s + "\""; }
    inline std::string obj(std::initializer_list<std::pair<const char*,std::string>> kv) {
        std::string r = "{";
        for (auto& p : kv) r += "\"" + std::string(p.first) + "\":" + p.second + ",";
        if (r.back() == ',') r.pop_back();
        return r + "}";
    }
    inline std::string get(const std::string& j, const std::string& key) {
        auto pos = j.find("\"" + key + "\"");
        if (pos == std::string::npos) return "";
        pos = j.find(':', pos) + 1;
        while (j[pos] == ' ') pos++;
        if (j[pos] == '"') {
            auto end = j.find('"', pos + 1);
            return j.substr(pos + 1, end - pos - 1);
        }
        auto end = j.find_first_of(",}", pos);
        return j.substr(pos, end - pos);
    }
}

// ── Frame protocol: 4-byte big-endian length prefix ──────────────────────────
static bool sendMsg(int fd, const std::string& msg) {
    uint32_t len = htonl(uint32_t(msg.size()));
    if (write(fd, &len, 4) != 4) return false;
    return write(fd, msg.data(), msg.size()) == (ssize_t)msg.size();
}
static std::string recvMsg(int fd) {
    uint32_t len = 0;
    if (read(fd, &len, 4) != 4) return "";
    len = ntohl(len);
    if (len > 65536) return "";
    std::string buf(len, '\0');
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, &buf[got], len - got);
        if (n <= 0) return "";
        got += n;
    }
    return buf;
}

// ── Handle one xApp connection ────────────────────────────────────────────────
static void handleXapp(int fd) {
    Logger::sys("[E2-Agent] xApp connected");

    // ── Step 1: E2 Setup ─────────────────────────────────────────
    std::string msg = recvMsg(fd);
    if (msg.empty()) { close(fd); return; }
    std::string type = je2::get(msg, "msgType");

    if (type == "E2SetupRequest") {
        Logger::step("E2 INTERFACE SETUP");
        Logger::sys("[E2-Agent] BEGINNER: An xApp (intelligent controller app) wants to "
                    "connect to this eNB to read RAN metrics and send control commands.");
        Logger::sys("[E2-Agent] ENGINEER: E2 Setup Request received. "
                    "E2AP procedure (O-RAN.WG3.E2AP §8.3.1). "
                    "E2 Node ID = " + je2::get(msg, "e2NodeId"));
        Logger::sys("[E2-Agent] INTERVIEW: What is O-RAN? O-RAN disaggregates traditional "
                    "RAN into open interfaces. E2 connects the Near-RT RIC (xApp host) "
                    "to the E2 Node (eNB/gNB). xApps implement ML-driven radio policies. "
                    "Alternative to closed vendor RAN software.");
        JsonEventLog::logEvent("xApp", "eNB", "E2 Subscription", "E2", 36421, "4g");

        std::string rsp = je2::obj({
            {"msgType", je2::str("E2SetupResponse")},
            {"transactionId", je2::str("1")},
            {"e2NodeId", je2::str("eNB-001")},
            {"cause", je2::str("success")},
        });
        sendMsg(fd, rsp);
        Logger::sys("[E2-Agent] → E2SetupResponse sent. E2 interface is UP.");
    }

    // ── Step 2: RIC Control ───────────────────────────────────────
    msg = recvMsg(fd);
    if (msg.empty()) { close(fd); return; }
    type = je2::get(msg, "msgType");

    if (type == "RICControlRequest") {
        std::string ueId  = je2::get(msg, "ueId");
        std::string action= je2::get(msg, "action");
        std::string value = je2::get(msg, "value");

        Logger::step("RIC CONTROL REQUEST  (xApp → eNB)");
        Logger::sys("[E2-Agent] BEGINNER: The intelligent controller (RIC) is telling this "
                    "eNB to reduce the data rate for UE #" + ueId + " to avoid interference.");
        Logger::sys("[E2-Agent] ENGINEER: RICControlRequest received. "
                    "RAN Function = RRC Control. Action = " + action + ". "
                    "Value = " + value + " (MCS index). "
                    "This would set the eNB's scheduler to cap UE " + ueId + " at MCS=" + value + ".");
        Logger::sys("[E2-Agent] INTERVIEW: Q: What is MCS in LTE and why would a RIC reduce it?");
        Logger::sys("[E2-Agent]   A: MCS (Modulation and Coding Scheme) is the mapping from "
                    "bits to radio symbols. MCS 0 = QPSK 1/8 (robust), MCS 28 = 64-QAM 5/6 (fast). "
                    "xApp reduces MCS for cell-edge UEs to reduce inter-cell interference — "
                    "a Coordinated Multi-Point (CoMP) scheduling policy impossible without O-RAN.");
        JsonEventLog::logEvent("xApp", "eNB", "E2 Control", "E2", 36421, "4g");

        // Simulate eNB applying the control action
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::string ack = je2::obj({
            {"msgType", je2::str("RICControlAcknowledge")},
            {"transactionId", je2::str("2")},
            {"ueId", je2::str(ueId)},
            {"result", je2::str("success")},
            {"appliedMCS", je2::str(value)},
        });
        sendMsg(fd, ack);
        Logger::step("RIC CONTROL ACKNOWLEDGE  (eNB → xApp)");
        Logger::sys("[E2-Agent] ENGINEER: RICControlAcknowledge sent. "
                    "eNB scheduler updated: UE " + ueId + " MCS capped at " + value + ".");
        Logger::sys("[E2-Agent] INTERVIEW: Q: How does the xApp know if the action worked?");
        Logger::sys("[E2-Agent]   A: After the control ack, the xApp reads E2 Indication messages "
                    "(RIC Subscription) to monitor the UE's actual PRB usage and SINR. "
                    "If SINR improves, the ML model updates its policy. This is the "
                    "closed-loop control cycle that makes O-RAN valuable.");
        JsonEventLog::logEvent("eNB", "xApp", "E2 Indication", "E2", 36421, "4g");
    }

    close(fd);
    Logger::sys("[E2-Agent] xApp disconnected — E2 session closed");
}

int main() {
    Logger::sys("[E2-Agent] O-RAN E2 Agent starting on port " + std::to_string(E2_PORT));
    Logger::sys("[E2-Agent] Waiting for xApp connection…  (run xapp_sim in another terminal)");

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1; setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(E2_PORT);
    bind(srv, (sockaddr*)&addr, sizeof(addr));
    listen(srv, 5);

    // Accept one xApp connection then exit
    int fd = accept(srv, nullptr, nullptr);
    close(srv);
    handleXapp(fd);

    Logger::shutdown();
    return 0;
}
