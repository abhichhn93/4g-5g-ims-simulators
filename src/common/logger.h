#pragma once
#include <iostream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <filesystem>
#include <string>
#include <cstdlib>

// ============================================================
// COLOR-CODED LOGGER — same pattern as the 4G simulator's
// src/common/logger.h, copied here so each 5G node binary is
// self-contained (one binary == one container == one pod).
//
// Node-to-node analogy (handy while reading logs side by side
// with the 4G project):
//   gNB <-> eNB     AMF <-> MME     UDM <-> HSS
//
// Each process picks its own session log file via setSessionFile()
// so gnb_sim / amf_sim / udm_sim don't stomp on each other's logs
// when run together.
// ============================================================
namespace Logger {

enum class Level {
    BEGINNER,    // High-level story
    ENGINEER,    // 3GPP IEs / message field dumps
    INTERVIEW_C, // C++ patterns / STL / memory
    INTERVIEW_T, // Telecom domain / 3GPP specs (now: 5G SBA)
    SYSTEM       // Errors and sockets
};

// ANSI colors — chosen to mirror the 4G simulator's analogous node:
//   gNB ~ eNB, AMF ~ MME, UDM ~ HSS, SMF ~ PCRF, UPF ~ S/P-GW
constexpr const char* CLR_GNB   = "\033[1;32m";    // Bold Green
constexpr const char* CLR_AMF   = "\033[1;34m";    // Bold Blue
constexpr const char* CLR_UDM   = "\033[1;33m";    // Bold Yellow
constexpr const char* CLR_SMF   = "\033[1;36m";    // Bold Cyan
constexpr const char* CLR_UPF   = "\033[38;5;208m";// Orange
constexpr const char* CLR_NRF   = "\033[1;35m";    // Bold Magenta
constexpr const char* CLR_SYS   = "\033[35m";      // Magenta
constexpr const char* CLR_WARN  = "\033[1;31m";    // Bold Red
constexpr const char* CLR_IE    = "\033[37m";      // White
constexpr const char* CLR_STEP  = "\033[1;37m";    // Bold White
constexpr const char* CLR_RESET = "\033[0m";

inline std::mutex& getMutex() { static std::mutex m; return m; }
inline Level& getGlobalLevel() { static Level l = Level::ENGINEER; return l; }
// When true (LOG_LEVEL=ALL), print() emits every level — used for the
// "deeper/engineering" mode that also dumps raw JSON via ie_field().
inline bool& getShowAll() { static bool b = false; return b; }

inline std::string& sessionFileName() {
    static std::string name = "g5_session.log";
    return name;
}
// Call once at the top of main() so each node writes its own log file,
// e.g. setSessionFile("g5_amf_session.log").
inline void setSessionFile(const std::string& name) { sessionFileName() = name; }

inline std::ofstream& getLogFile() {
    static std::ofstream file;
    if (!file.is_open()) {
        file.open(sessionFileName(), std::ios::out | std::ios::trunc);
    }
    return file;
}

inline void shutdown() {
    std::lock_guard<std::mutex> lk(getMutex());
    if (getLogFile().is_open()) {
        getLogFile().close();
        std::cout << CLR_SYS << "\n[SYS] Session log saved: "
                  << std::filesystem::absolute(sessionFileName()).string() << CLR_RESET << std::endl;
    }
}

inline std::string now() {
    auto tp = std::chrono::system_clock::now();
    auto t  = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) % 1000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

inline void setLevel(Level l) { getGlobalLevel() = l; }

// Call once at the top of main(). Reads LOG_LEVEL env var:
//   ENGINEER (default) | INTERVIEW_C | INTERVIEW_T | ALL
// ALL turns on showAll, which prints every level — the "deeper/engineering
// mode" that also surfaces the raw JSON via ie_field("raw: ...").
inline void setLevelFromEnv() {
    const char* env = std::getenv("LOG_LEVEL");
    std::string lvl = env ? std::string(env) : "ENGINEER";
    if (lvl == "ALL") {
        getShowAll() = true;
        getGlobalLevel() = Level::ENGINEER;
    } else if (lvl == "INTERVIEW_C") {
        getGlobalLevel() = Level::INTERVIEW_C;
    } else if (lvl == "INTERVIEW_T") {
        getGlobalLevel() = Level::INTERVIEW_T;
    } else {
        getGlobalLevel() = Level::ENGINEER;
    }
}

inline void print(Level level, const char* color, const char* tag, const std::string& msg) {
    // BEGINNER (the call-flow narrative: "AMF -> gNB: RegistrationAccept")
    // and SYSTEM always print -- without this, the story-level flow was
    // silently dropped whenever the global level was ENGINEER (the default).
    bool always = (level == Level::SYSTEM || level == Level::BEGINNER);
    if (!always && !getShowAll() && level != getGlobalLevel()) return;

    std::lock_guard<std::mutex> lk(getMutex());

    std::string prefix = "";
    if (level == Level::BEGINNER)    prefix = "[STORY] ";
    if (level == Level::INTERVIEW_C) prefix = "[C++ DESIGN] ";
    if (level == Level::INTERVIEW_T) prefix = "[5G SBA] ";
    if (level == Level::ENGINEER)    prefix = "[5GC] ";

    std::cout << color << "[" << now() << "][" << tag << "] "
              << prefix << msg << CLR_RESET << std::endl; // flush: 'kubectl/docker logs -f' need live output

    auto& f = getLogFile();
    if (f.is_open()) {
        f << "[" << now() << "][" << tag << "] " << prefix << msg << std::endl;
    }
}

// IE / field detail line — indented, no timestamp (part of previous log)
inline void ie_field(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_IE << "         │  " << msg << CLR_RESET << std::endl;
    auto& f = getLogFile();
    if (f.is_open()) f << "         │  " << msg << std::endl;
}

// "Deeper/engineering mode": dumps the raw JSON text of an N2/SBI
// message. Only shown with LOG_LEVEL=ALL or LOG_LEVEL=INTERVIEW_C --
// this is the byte-structure detail for "explain registration" interviews.
inline void raw(const std::string& text) {
    if (getShowAll() || getGlobalLevel() == Level::INTERVIEW_C)
        ie_field("raw: " + text);
}

// Step banner — bold separator for major call-flow steps
inline void step(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_STEP << "\n  ══ " << msg << " ══\n" << CLR_RESET << std::flush;
}

// Per-node log functions
inline void gnb(Level l, const std::string& m) { print(l, CLR_GNB, " gNB  ", m); }
inline void amf(Level l, const std::string& m) { print(l, CLR_AMF, " AMF  ", m); }
inline void udm(Level l, const std::string& m) { print(l, CLR_UDM, " UDM  ", m); }
inline void smf(Level l, const std::string& m) { print(l, CLR_SMF, " SMF  ", m); }
inline void upf(Level l, const std::string& m) { print(l, CLR_UPF, " UPF  ", m); }
inline void nrf(Level l, const std::string& m) { print(l, CLR_NRF, " NRF  ", m); }
inline void sys(const std::string& m) { print(Level::SYSTEM, CLR_SYS, " SYS  ", m); }
inline void warn(const std::string& tag, const std::string& m) {
    print(Level::SYSTEM, CLR_WARN, tag.c_str(), "ERROR: " + m);
}

} // namespace Logger
