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
#include <thread>
#include <cstdlib>

// ============================================================
// COLOR-CODED LOGGER with THREAD NAMES
//
// Every log line shows:
//   [HH:MM:SS.mmm][NODE][thread:NAME] message
//
// NODE colours:
//   Green  = CBSD agent (the radio device)
//   Blue   = Domain Proxy (the middlebox)
//   Yellow = SAS (the cloud authority)
//   Magenta = system / lifecycle events
//
// Thread names are set once per thread with Logger::setThreadName().
// All other threads get "thread-NNN" from a short hash of thread::id.
// ============================================================
namespace Logger {

enum class Level {
    BEGINNER,    // High-level story: what is happening and why
    ENGINEER,    // Protocol field details (IEs, state changes, mutex events)
    INTERVIEW_C, // C++ design pattern explanations
    INTERVIEW_T, // CBRS / spectrum-sharing domain knowledge
    SYSTEM       // Socket errors and lifecycle events (always printed)
};

constexpr const char* CLR_CBSD  = "\033[1;32m";
constexpr const char* CLR_DP    = "\033[1;34m";
constexpr const char* CLR_SAS   = "\033[1;33m";
constexpr const char* CLR_SYS   = "\033[35m";
constexpr const char* CLR_WARN  = "\033[1;31m";
constexpr const char* CLR_IE    = "\033[37m";
constexpr const char* CLR_STEP  = "\033[1;37m";
constexpr const char* CLR_RESET = "\033[0m";

inline std::mutex&  getMutex()    { static std::mutex m;               return m; }
inline Level&       getLevel()    { static Level l = Level::ENGINEER;  return l; }
inline bool&        getShowAll()  { static bool b  = false;            return b; }
inline std::string& sessionFile() { static std::string s = "cbrs_session.log"; return s; }

// ── Thread name — set once per thread, visible in every log line ──────────
// thread_local: each thread has its own copy of this string.
// Without this, all log lines would say "unknown-thread" or a hash.
inline thread_local std::string tl_thread_name;

inline void setThreadName(const std::string& name) {
    tl_thread_name = name;
}

inline std::string threadName() {
    if (!tl_thread_name.empty()) return tl_thread_name;
    // fallback: short hash of thread id so it's still unique but short
    std::size_t h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    char buf[12]; std::snprintf(buf, sizeof(buf), "t-%04zu", h % 10000);
    return buf;
}

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
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  tp.time_since_epoch()) % 1000;
    std::ostringstream ss;
    ss << std::put_time(std::localtime(&t), "%H:%M:%S")
       << "." << std::setfill('0') << std::setw(3) << ms.count();
    return ss.str();
}

inline void setLevelFromEnv() {
    const char* env = std::getenv("LOG_LEVEL");
    std::string lvl = env ? std::string(env) : "ENGINEER";
    if      (lvl == "ALL")         { getShowAll() = true; }
    else if (lvl == "BEGINNER")    { getLevel() = Level::BEGINNER; }
    else if (lvl == "INTERVIEW_C") { getLevel() = Level::INTERVIEW_C; }
    else if (lvl == "INTERVIEW_T") { getLevel() = Level::INTERVIEW_T; }
    else                           { getLevel() = Level::ENGINEER; }
}

inline void print(Level level, const char* color, const char* tag, const std::string& msg) {
    bool always = (level == Level::SYSTEM || level == Level::BEGINNER);
    if (!always && !getShowAll() && level != getLevel()) return;

    std::string tn = threadName();
    // pad thread name to 12 chars so columns align
    std::string tnPad = tn;
    if (tnPad.size() < 12) tnPad.resize(12, ' ');

    std::lock_guard<std::mutex> lk(getMutex());
    std::ostringstream line;
    line << "[" << now() << "][" << tag << "][" << tnPad << "] " << msg;

    std::cout << color << line.str() << CLR_RESET << "\n";
    auto& f = getLogFile();
    if (f.is_open()) f << line.str() << "\n";
}

// ie_field: indented field line, no thread prefix (it's a continuation)
inline void ie_field(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::string line = "                                  │  " + msg;
    std::cout << CLR_IE << line << CLR_RESET << "\n";
    auto& f = getLogFile();
    if (f.is_open()) f << line << "\n";
}

inline void step(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::string line = "\n  ══ " + msg + " ══\n";
    std::cout << CLR_STEP << line << CLR_RESET << std::flush;
}

// ── Per-node convenience wrappers ────────────────────────────────────────
inline void cbsd(Level l, const std::string& m) { print(l, CLR_CBSD, " CBSD ", m); }
inline void dp  (Level l, const std::string& m) { print(l, CLR_DP,   "  DP  ", m); }
inline void sas (Level l, const std::string& m) { print(l, CLR_SAS,  " SAS  ", m); }
inline void sys (const std::string& m)           { print(Level::SYSTEM, CLR_SYS, " SYS  ", m); }
inline void warn(const std::string& tag, const std::string& m) {
    print(Level::SYSTEM, CLR_WARN, (" " + tag + " ").c_str(), "ERROR: " + m);
}

} // namespace Logger
