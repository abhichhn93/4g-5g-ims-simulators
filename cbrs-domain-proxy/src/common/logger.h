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

// Color-coded logger — same pattern as the 4G/5G simulators in this repo.
//
// CBRS node analogy (helps when reading logs):
//   CBSD  <->  gNB / UE    (the radio device at the "edge")
//   Domain Proxy  <->  MME / AMF  (the aggregating middlebox)
//   SAS  <->  HSS / UDM    (the authoritative backend)
namespace Logger {

enum class Level {
    BEGINNER,    // High-level story: what is happening and why
    ENGINEER,    // Protocol field details (message IEs, state changes)
    INTERVIEW_C, // C++ design patterns used in this file
    INTERVIEW_T, // CBRS / spectrum-sharing domain knowledge
    SYSTEM       // Socket errors and lifecycle events
};

constexpr const char* CLR_CBSD  = "\033[1;32m";     // Bold Green  — CBSD agent
constexpr const char* CLR_DP    = "\033[1;34m";     // Bold Blue   — Domain Proxy
constexpr const char* CLR_SAS   = "\033[1;33m";     // Bold Yellow — SAS stub
constexpr const char* CLR_SYS   = "\033[35m";       // Magenta     — system/lifecycle
constexpr const char* CLR_WARN  = "\033[1;31m";     // Bold Red    — errors
constexpr const char* CLR_IE    = "\033[37m";       // White       — field details
constexpr const char* CLR_STEP  = "\033[1;37m";     // Bold White  — step banners
constexpr const char* CLR_RESET = "\033[0m";

inline std::mutex& getMutex()       { static std::mutex m;         return m; }
inline Level&      getLevel()       { static Level l = Level::ENGINEER; return l; }
inline bool&       getShowAll()     { static bool b = false;        return b; }
inline std::string& sessionFile()   { static std::string s = "cbrs_session.log"; return s; }

inline void setSessionFile(const std::string& name) { sessionFile() = name; }

inline std::ofstream& getLogFile() {
    static std::ofstream f;
    if (!f.is_open()) f.open(sessionFile(), std::ios::out | std::ios::trunc);
    return f;
}

inline void shutdown() {
    std::lock_guard<std::mutex> lk(getMutex());
    if (getLogFile().is_open()) {
        getLogFile().close();
        std::cout << CLR_SYS << "\n[SYS] Session log: "
                  << std::filesystem::absolute(sessionFile()).string()
                  << CLR_RESET << std::endl;
    }
}

inline std::string now() {
    auto tp = std::chrono::system_clock::now();
    auto t  = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

inline void setLevelFromEnv() {
    const char* env = std::getenv("LOG_LEVEL");
    std::string lvl = env ? std::string(env) : "ENGINEER";
    if      (lvl == "ALL")         { getShowAll() = true; getLevel() = Level::ENGINEER; }
    else if (lvl == "INTERVIEW_C") { getLevel() = Level::INTERVIEW_C; }
    else if (lvl == "INTERVIEW_T") { getLevel() = Level::INTERVIEW_T; }
    else if (lvl == "BEGINNER")    { getLevel() = Level::BEGINNER; }
    else                           { getLevel() = Level::ENGINEER; }
}

inline void print(Level level, const char* color, const char* tag, const std::string& msg) {
    bool always = (level == Level::SYSTEM || level == Level::BEGINNER);
    if (!always && !getShowAll() && level != getLevel()) return;

    std::lock_guard<std::mutex> lk(getMutex());

    std::string prefix;
    if (level == Level::BEGINNER)    prefix = "[STORY] ";
    if (level == Level::INTERVIEW_C) prefix = "[C++ DESIGN] ";
    if (level == Level::INTERVIEW_T) prefix = "[CBRS SPEC] ";
    if (level == Level::ENGINEER)    prefix = "[CBRS] ";

    std::cout << color << "[" << now() << "][" << tag << "] "
              << prefix << msg << CLR_RESET << std::endl;

    auto& f = getLogFile();
    if (f.is_open()) f << "[" << now() << "][" << tag << "] " << prefix << msg << std::endl;
}

inline void ie_field(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_IE << "         │  " << msg << CLR_RESET << std::endl;
    auto& f = getLogFile();
    if (f.is_open()) f << "         │  " << msg << std::endl;
}

inline void step(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_STEP << "\n  ══ " << msg << " ══\n" << CLR_RESET << std::flush;
}

// Per-node convenience functions
inline void cbsd(Level l, const std::string& m) { print(l, CLR_CBSD, " CBSD ", m); }
inline void dp  (Level l, const std::string& m) { print(l, CLR_DP,   "  DP  ", m); }
inline void sas (Level l, const std::string& m) { print(l, CLR_SAS,  " SAS  ", m); }
inline void sys (const std::string& m)           { print(Level::SYSTEM, CLR_SYS, " SYS  ", m); }
inline void warn(const std::string& tag, const std::string& m) {
    print(Level::SYSTEM, CLR_WARN, tag.c_str(), "ERROR: " + m);
}

} // namespace Logger
