// chaos_mode.h — toggleable fault injection for resilience demos.
// CHAOS ON → 20% probability of dropping/corrupting one message per flow.
// This simulates real network failures (HSS overload, UDP loss, SIP 4xx).
// Toggle with CLI command: CHAOS on|off
// Thread-safe: uses std::atomic<bool>.
#pragma once
#include <atomic>
#include <cstdlib>
#include <functional>
#include <string>
#include "common/logger.h"

namespace Chaos {

inline std::atomic<bool>& enabled() {
    static std::atomic<bool> s{false};
    return s;
}

inline void setEnabled(bool on) {
    enabled().store(on);
    if (on)
        Logger::sys("[CHAOS] CHAOS MODE ON — 20% random message drop/corrupt probability");
    else
        Logger::sys("[CHAOS] CHAOS MODE OFF — normal operation resumed");
}

// Returns true (and logs the chaos event) 20% of the time when chaos is ON.
// `msg`  = description of what was dropped (e.g. "Diameter AIA from HSS")
// `from` = source node
// `to`   = dest node
// `recovery` = what the system would do in a real network to recover
inline bool rollDrop(const std::string& msg,
                     const std::string& from,
                     const std::string& to,
                     const std::string& recovery) {
    if (!enabled().load()) return false;
    if ((std::rand() % 100) >= 20) return false;  // 80% pass-through

    Logger::sys("[CHAOS] ★ FAULT INJECTED: dropped " + msg +
                "  (" + from + "→" + to + ")");
    Logger::sys("[CHAOS]   BEGINNER: The " + from +
                " message to " + to + " was silently lost — this happens in real networks!");
    Logger::sys("[CHAOS]   ENGINEER: 20% packet-loss scenario. No ACK received within T3 timer.");
    Logger::sys("[CHAOS]   INTERVIEW: This is how real networks handle node failures —");
    Logger::sys("[CHAOS]   " + recovery);
    return true;
}

// Corrupt variant: message arrives but with bad content (e.g. wrong checksum)
inline bool rollCorrupt(const std::string& msg,
                        const std::string& from,
                        const std::string& to,
                        const std::string& recovery) {
    if (!enabled().load()) return false;
    if ((std::rand() % 100) >= 20) return false;

    Logger::sys("[CHAOS] ★ CORRUPTION INJECTED: corrupted " + msg +
                "  (" + from + "→" + to + ")");
    Logger::sys("[CHAOS]   ENGINEER: RAND/AUTN or SIP body has one bit flipped.");
    Logger::sys("[CHAOS]   INTERVIEW: Receiver detects checksum mismatch → " + recovery);
    return true;
}

} // namespace Chaos
