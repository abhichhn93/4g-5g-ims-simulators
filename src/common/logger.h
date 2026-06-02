#pragma once
#include <iostream>
#include <mutex>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
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

inline void print(const char* color, const char* tag, const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << color << "[" << now() << "][" << tag << "] "
              << msg << CLR_RESET << "\n";
}

// IE detail line — indented, white, no timestamp (part of previous log)
inline void ie_field(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_IE << "         │  " << msg << CLR_RESET << "\n";
}

// Step banner — bold separator for major call flow steps
inline void step(const std::string& msg) {
    std::lock_guard<std::mutex> lk(getMutex());
    std::cout << CLR_STEP
              << "\n  ══ " << msg << " ══\n"
              << CLR_RESET;
}

// Per-node log functions
inline void enb (const std::string& m) { print(CLR_ENB,   " eNB  ", m); }
inline void mme (const std::string& m) { print(CLR_MME,   " MME  ", m); }
inline void hss (const std::string& m) { print(CLR_HSS,   " HSS  ", m); }
inline void pcrf(const std::string& m) { print(CLR_PCRF,  " PCRF ", m); }
inline void sgw (const std::string& m) { print(CLR_SGW,   " S-GW ", m); }
inline void pgw (const std::string& m) { print(CLR_PGW,   " P-GW ", m); }
inline void pcscf(const std::string& m){ print(CLR_PCSCF, "P-CSCF", m); }
inline void scscf(const std::string& m){ print(CLR_SCSCF, "S-CSCF", m); }
inline void mtas(const std::string& m) { print(CLR_MTAS,  " MTAS ", m); }
inline void sys (const std::string& m) { print(CLR_SYS,   " SYS  ", m); }
inline void warn(const std::string& tag, const std::string& m) {
    print(CLR_WARN, tag.c_str(), "ERROR: " + m);
}

} // namespace Logger
