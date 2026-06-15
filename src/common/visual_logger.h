#pragma once
// ============================================================
// VISUAL LOGGER — Gemini suggestion implemented
//
// Problem with naive Logger: multiple node threads call
// std::cout concurrently → ANSI borders shatter/interleave.
//
// Solution: buffer the ENTIRE step block in std::stringstream,
// then flush to std::cout in ONE lock cycle. Zero interleaving.
//
// INTERVIEW: "How do you prevent torn log lines in concurrent systems?"
//   "I use a per-block stringstream. Each thread builds its entire
//    output locally (no locking needed during build), then acquires
//    the global mutex once and writes the whole block atomically.
//    This is lock-free during formatting and minimizes contention."
// ============================================================
#include <sstream>
#include <string>
#include <iostream>
#include <thread>
#include <functional>
#include "common/logger.h"

namespace VLog {

// ── ANSI helpers ─────────────────────────────────────────────
static constexpr const char* BOLD  = "\033[1;37m";
static constexpr const char* DIM   = "\033[37m";
static constexpr const char* GREEN = "\033[1;32m";
static constexpr const char* RST   = "\033[0m";
static constexpr int  WIDTH = 62;  // content width inside borders

inline std::string tid() {
    // Short thread ID — last 5 digits of hash (readable, unique enough)
    auto h = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return "T-" + std::to_string(h % 100000);
}

inline std::string pad(const std::string& s, int w) {
    if ((int)s.size() >= w) return s.substr(0, w);
    return s + std::string(w - s.size(), ' ');
}
// repeat a multi-byte UTF-8 string n times (e.g. "═" × 60)
inline std::string rep(const std::string& s, int n) {
    std::string r; r.reserve(s.size() * n);
    for (int i = 0; i < n; ++i) r += s;
    return r;
}

// ── StepBlock ─────────────────────────────────────────────────
// Fluent builder. Entire block rendered into buf_, then one-shot
// flush under the global Logger mutex.
class StepBlock {
public:
    StepBlock(int step, int total,
              const std::string& msg_name,
              const std::string& from, const char* from_clr,
              const std::string& to,   const char* to_clr) {
        // ── Top border ────────────────────────────────────────
        std::string hdr = " STEP " + std::to_string(step) + "/" +
                          std::to_string(total) + "  " + msg_name;
        std::string t_id = "  [" + tid() + "] ";
        buf_ << "\n" << BOLD
             << "  ╔" << rep("=", WIDTH) << "╗\n"
             << "  ║" << pad(hdr, WIDTH - (int)t_id.size())
             << t_id << "║\n";

        // ── Arrow line ────────────────────────────────────────
        // e.g.  "  UE ───────────────────────────────────► MME"
        int arrow_space = WIDTH - 2 - (int)from.size() - (int)to.size();
        if (arrow_space < 4) arrow_space = 4;
        std::string arrow = std::string(arrow_space - 1, '-') + "►";
        buf_ << RST
             << "  ║ " << from_clr << from << RST
             << DIM << arrow << RST
             << to_clr << to << RST << BOLD << " ║\n";

        // ── Divider ───────────────────────────────────────────
        buf_ << BOLD << "  ╠" << rep("-", WIDTH) << "╣\n" << RST;
    }

    // Add an IE field (key: value)
    StepBlock& ie(const std::string& key, const std::string& val) {
        std::string line = "  " + key + ": " + val;
        buf_ << DIM << "  ║ " << pad(line, WIDTH - 1) << "║\n" << RST;
        return *this;
    }

    // Show state transition: [Node State: OLD → NEW]
    StepBlock& state(const std::string& node, const std::string& trans) {
        std::string line = "  [" + node + ": " + trans + "]";
        buf_ << GREEN << "  ║ " << pad(line, WIDTH - 1) << "║\n" << RST;
        return *this;
    }

    // Divider before "Next" hint
    StepBlock& next(const std::string& hint) {
        buf_ << BOLD << "  ╠" << rep("-", WIDTH) << "╣\n" << RST;
        std::string line = "  ▶ Next: " + hint;
        buf_ << DIM << "  ║ " << pad(line, WIDTH - 1) << "║\n" << RST;
        buf_ << BOLD << "  ╚" << rep("=", WIDTH) << "╝\n" << RST;
        flushed_next_ = true;
        return *this;
    }

    void flush() {
        if (!flushed_next_) {
            buf_ << BOLD << "  ╚" << rep("=", WIDTH) << "╝\n" << RST;
        }
        std::lock_guard<std::mutex> lk(Logger::getMutex());
        std::cout << buf_.str() << std::flush;
        flushed_ = true;
    }

    ~StepBlock() { if (!flushed_) flush(); }

private:
    std::ostringstream buf_;
    bool flushed_      = false;
    bool flushed_next_ = false;
};

// ── Startup diagram ───────────────────────────────────────────
inline void printStartupDiagram() {
    std::lock_guard<std::mutex> lk(Logger::getMutex());
    std::cout << RST << "\n"
        << BOLD << "  +============================================================+\n"
        << "  |     4G EPC + IMS/VoLTE SIMULATOR  (C++17)                |\n"
        << "  |     github.com/YOUR_USERNAME/ims-simulator                |\n"
        << "  +============================================================+\n" << RST
        << "\n"
        << Logger::CLR_ENB  << "  ┌──────┐\n"
        << "  │  UE  │  (simulated by main thread)\n"
        << "  └──┬───┘\n" << RST
        << DIM   << "       │ S1AP (port 36412)\n" << RST
        << Logger::CLR_ENB  << "  ┌──────┴───┐\n"
        << "  │   eNB    │  eNodeB — radio access node\n"
        << "  └──────────┘\n" << RST
        << DIM   << "       │ S1AP\n" << RST
        << Logger::CLR_MME  << "  ┌──────────┐      Diameter S6a         ┌─────┐\n"
        << "  │   MME    │ ─────────────────────────► │ HSS │\n"
        << "  └────┬─────┘  (Authentication, Profile) └─────┘\n" << RST
        << DIM    << "       │ GTP-Cv2 S11 (port 2123)\n" << RST
        << Logger::CLR_SGW  << "  ┌──────────┐\n"
        << "  │   S-GW   │  Serving Gateway\n"
        << "  └────┬─────┘\n" << RST
        << DIM    << "       │ GTP-Cv2 S5 (port 2124)\n" << RST
        << Logger::CLR_PGW  << "  ┌──────────┐      Diameter Gx          ┌──────┐\n"
        << "  │   P-GW   │ ─────────────────────────► │ PCRF │\n"
        << "  └──────────┘  (QCI Policy, Bearer auth) └──────┘\n" << RST
        << "\n"
        << Logger::CLR_PCSCF << "  IMS: P-CSCF(5060) → S-CSCF+MTAS(5070) → IMS-HSS(3870)\n" << RST
        << "\n"
        << BOLD << "  Ports: eNB:38412 HSS:3868 SGW:2123 PGW:2124 PCRF:3869\n"
        << "  PCAP: packets written to mme_capture.pcap (open in Wireshark)\n"
        << "  Commands: CR 1  │  BULK 5  │  STATUS  │  QUIT\n"
        << "  ──────────────────────────────────────────────────────────\n" << RST
        << "\n" << std::flush;
}

// ── Shorthand factory ─────────────────────────────────────────
inline StepBlock step(int n, int total, const std::string& msg,
                      const std::string& from, const char* fc,
                      const std::string& to,   const char* tc) {
    return StepBlock(n, total, msg, from, fc, to, tc);
}

} // namespace VLog
