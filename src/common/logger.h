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

// ============================================================
// COLOR-CODED LOGGER — every node has its own ANSI color
//
// Thread-safe via singleton mutex. Each node's prints are
// distinct colors so you can visually trace packets flowing
// between nodes in real-time.
//
// INTERVIEW: "How do you debug concurrent systems?"
//   - Structured logging with ms-precision timestamps
//   - Color per node so you visually see message flow
//   - Log every state transition and IE field
// ============================================================
namespace Logger {

enum class Level {
    BEGINNER,    // High-level story
    ENGINEER,    // 3GPP IEs and Hex decodes
    INTERVIEW_C, // C++ Patterns / STL / Memory
    INTERVIEW_T, // Telecom Domain / 3GPP Specs
    SYSTEM       // Errors and Sockets
};

// ANSI colors — Bold variants for node headers, normal for IEs
constexpr const char* CLR_ENB    = "\033[1;32m";    // Bold Green    — eNB
constexpr const char* CLR_MME    = "\033[1;34m";    // Bold Blue     — MME
constexpr const char* CLR_HSS    = "\033[1;33m";    // Bold Yellow   — HSS
constexpr const char* CLR_PCRF   = "\033[1;36m";    // Bold Cyan     — PCRF
constexpr const char* CLR_SGW    = "\033[1;35m";    // Bold Magenta  — S-GW
constexpr const char* CLR_PGW    = "\033[38;5;208m";// Orange        — P-GW
constexpr const char* CLR_PCSCF  = "\033[1;96m";    // Bright Cyan   — P-CSCF
constexpr const char* CLR_SCSCF  = "\033[1;94m";    // Bright Blue   — S-CSCF
constexpr const char* CLR_MTAS   = "\033[1;93m";    // Bright Yellow — MTAS/AS
constexpr const char* CLR_SYS    = "\033[35m";      // Magenta       — SYSTEM
constexpr const char* CLR_WARN   = "\033[1;31m";    // Bold Red      — ERROR
constexpr const char* CLR_IE     = "\033[37m";      // White         — IE fields
constexpr const char* CLR_STEP   = "\033[1;37m";    // Bold White    — step banners
constexpr const char* CLR_RESET  = "\033[0m";

inline std::mutex& getMutex() { static std::mutex m; return m; }
inline Level& getGlobalLevel() { static Level l = Level::ENGINEER; return l; }

inline std::ofstream& getLogFile() {
    static std::ofstream file;
    if (!file.is_open()) {
        file.open("mme_sim_session.log", std::ios::out | std::ios::trunc);
    }
    return file;
}

inline void shutdown() {
    std::lock_guard<std::mutex> lk(getMutex());
    if (getLogFile().is_open()) {
        getLogFile().close();
        std::cout << CLR_SYS << "\n[SYS] Session logs successfully archived at: " 
                  << std::filesystem::absolute("mme_sim_session.log").string() << CLR_RESET << std::endl;
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

inline void print(Level level, const char* color, const char* tag, const std::string& msg) {
    if (level != Level::SYSTEM && level != getGlobalLevel()) return;
    
    std::lock_guard<std::mutex> lk(getMutex());
    
    std::string prefix = "";
    if (level == Level::BEGINNER) prefix = "[STORY] ";
    if (level == Level::INTERVIEW_C) prefix = "[C++ DESIGN] ";
    if (level == Level::INTERVIEW_T) prefix = "[3GPP LOGIC] ";
    if (level == Level::ENGINEER) prefix = "[3GPP] ";

    std::cout << color << "[" << now() << "][" << tag << "] "
              << prefix << msg << CLR_RESET << "\n";

    // Persist to file (without ANSI colors for readability)
    auto& f = getLogFile();
    if (f.is_open()) {
        f << "[" << now() << "][" << tag << "] " << prefix << msg << "\n";
    }
}

// IE detail line — indented, white, no timestamp (part of previous log)
inline void ie_field(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_IE << "         │  " << msg << CLR_RESET << "\n";
    auto& f = getLogFile();
    if (f.is_open()) {
        f << "         │  " << msg << "\n";
    }
}

// Step banner — bold separator for major call flow steps
inline void step(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_STEP
              << "\n  ══ " << msg << " ══\n"
              << CLR_RESET;
}

// Per-node log functions
inline void enb (Level l, const std::string& m) { print(l, CLR_ENB,   " eNB  ", m); }
inline void mme (Level l, const std::string& m) { print(l, CLR_MME,   " MME  ", m); }
inline void hss (Level l, const std::string& m) { print(l, CLR_HSS,   " HSS  ", m); }
inline void pcrf(Level l, const std::string& m) { print(l, CLR_PCRF,  " PCRF ", m); }
inline void sgw (Level l, const std::string& m) { print(l, CLR_SGW,   " S-GW ", m); }
inline void pgw (Level l, const std::string& m) { print(l, CLR_PGW,   " P-GW ", m); }
inline void pcscf(Level l, const std::string& m){ print(l, CLR_PCSCF, "P-CSCF", m); }
inline void scscf(Level l, const std::string& m){ print(l, CLR_SCSCF, "S-CSCF", m); }
inline void mtas(Level l, const std::string& m) { print(l, CLR_MTAS,  " MTAS ", m); }
inline void sys (const std::string& m) { print(Level::SYSTEM, CLR_SYS, " SYS  ", m); }
inline void warn(const std::string& tag, const std::string& m) {
    print(Level::SYSTEM, CLR_WARN, tag.c_str(), "ERROR: " + m);
}

} // namespace Logger
