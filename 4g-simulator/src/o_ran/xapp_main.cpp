// xapp_main.cpp — O-RAN xApp that connects to the eNB's E2 Agent.
// Sends: E2SetupRequest → RICControlRequest (reduce MCS for UE)
// Receives: E2SetupResponse → RICControlAcknowledge
// Port 36421 (E2 interface). Run after e2_agent: ./xapp_sim
// Build: see CMakeLists.txt target xapp_sim
#include <iostream>
#include <string>
#include <cstring>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include "common/logger.h"
#include "common/json_event_log.h"

static constexpr const char*  E2_HOST = "127.0.0.1";
static constexpr uint16_t     E2_PORT = 36421;

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

int main(int argc, char* argv[]) {
    int ueId  = 1;
    int mcs   = 10;   // target MCS (0-28)
    if (argc >= 2) ueId = std::stoi(argv[1]);
    if (argc >= 3) mcs  = std::stoi(argv[2]);

    Logger::sys("[xApp] O-RAN Near-RT RIC xApp starting");
    Logger::sys("[xApp] BEGINNER: This is an intelligent controller that will connect to the "
                "eNB and tell it to adjust radio settings for UE " + std::to_string(ueId));
    Logger::sys("[xApp] ENGINEER: Connecting to E2 Agent at " +
                std::string(E2_HOST) + ":" + std::to_string(E2_PORT));
    Logger::sys("[xApp] INTERVIEW: Q: What is a Near-RT RIC and how is it different from Non-RT RIC?");
    Logger::sys("[xApp]   A: Near-RT RIC (10ms–1s control loop) hosts xApps that make fast "
                "per-UE/per-cell decisions via E2 interface. "
                "Non-RT RIC (>1s) hosts rApps that set policies via A1 interface. "
                "Both are open: vendors compete on xApp algorithms, not locked-in hardware.");

    // ── Connect to E2 Agent ───────────────────────────────────────
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(E2_PORT);
    inet_pton(AF_INET, E2_HOST, &addr.sin_addr);
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        Logger::warn(" xApp ", "Cannot connect to E2 Agent — start e2_agent first");
        return 1;
    }
    Logger::sys("[xApp] Connected to E2 Agent");

    // ── Step 1: E2 Setup ─────────────────────────────────────────
    Logger::step("E2 SETUP REQUEST  (xApp → eNB E2 Agent)");
    Logger::sys("[xApp] ENGINEER: Sending E2SetupRequest — lists supported RAN Functions "
                "(RC = RAN Control, SM = Slice Management, CCC = Cell Config Control)");
    std::string setup = je2::obj({
        {"msgType",    je2::str("E2SetupRequest")},
        {"e2NodeId",   je2::str("xApp-RIC-001")},
        {"ranFunctions", "[\"RC\",\"SM\",\"CCC\"]"},
    });
    sendMsg(fd, setup);
    JsonEventLog::logEvent("xApp", "eNB", "E2 Subscription", "E2", E2_PORT, "4g");

    std::string rsp = recvMsg(fd);
    if (rsp.empty() || je2::get(rsp, "msgType") != "E2SetupResponse") {
        Logger::warn(" xApp ", "E2 Setup failed: " + rsp);
        close(fd); return 1;
    }
    Logger::step("E2 SETUP RESPONSE  (eNB → xApp)");
    Logger::sys("[xApp] E2 interface UP — xApp can now send control commands to eNB");
    Logger::sys("[xApp] ENGINEER: E2 Node ID confirmed = " + je2::get(rsp, "e2NodeId"));

    // ── Step 2: RIC Control Request ───────────────────────────────
    Logger::step("RIC CONTROL REQUEST  (xApp → eNB)");
    Logger::sys("[xApp] BEGINNER: Sending control command to reduce UE " +
                std::to_string(ueId) + "'s transmission speed (MCS=" + std::to_string(mcs) +
                ") to reduce interference with neighbouring cells.");
    Logger::sys("[xApp] ENGINEER: RICControlRequest with action=reduceMCS, ueId=" +
                std::to_string(ueId) + ", MCS=" + std::to_string(mcs) +
                " (E2AP §8.4.2). The eNB scheduler will apply this as a per-UE MCS cap.");
    Logger::sys("[xApp] INTERVIEW: Q: Why is reducing MCS useful for interference management?");
    Logger::sys("[xApp]   A: Lower MCS (more robust modulation + more coding) uses more "
                "time-frequency resources per bit, BUT reduces the transmitted signal power "
                "relative to data rate. For cell-edge UEs, CoMP/ICIC algorithms reduce MCS "
                "to cut interference into neighbouring cells (SINR improves for all).");

    std::string ctrl = je2::obj({
        {"msgType",       je2::str("RICControlRequest")},
        {"transactionId", je2::str("2")},
        {"ueId",          je2::str(std::to_string(ueId))},
        {"action",        je2::str("reduceMCS")},
        {"value",         je2::str(std::to_string(mcs))},
        {"rationale",     je2::str("SINR-below-threshold-A3-event")},
    });
    sendMsg(fd, ctrl);
    JsonEventLog::logEvent("xApp", "eNB", "E2 Control", "E2", E2_PORT, "4g");

    std::string ack = recvMsg(fd);
    if (ack.empty()) {
        Logger::warn(" xApp ", "No control ack received"); close(fd); return 1;
    }
    Logger::step("RIC CONTROL ACKNOWLEDGE  (eNB → xApp)");
    Logger::sys("[xApp] BEGINNER: The eNB confirmed the radio setting change — "
                "UE " + std::to_string(ueId) + " now has MCS=" +
                je2::get(ack, "appliedMCS") + " applied.");
    Logger::sys("[xApp] ENGINEER: RICControlAcknowledge received. "
                "Result = " + je2::get(ack, "result") + ". "
                "Applied MCS = " + je2::get(ack, "appliedMCS"));
    Logger::sys("[xApp] Next: subscribe to E2 Indication (RIC Subscription) to "
                "monitor PRB usage and SINR to close the control loop.");
    JsonEventLog::logEvent("eNB", "xApp", "E2 Indication", "E2", E2_PORT, "4g");

    Logger::sys("[xApp] O-RAN E2 demo complete ✓");
    close(fd);
    Logger::shutdown();
    return 0;
}
