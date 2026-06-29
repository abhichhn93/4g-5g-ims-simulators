// ============================================================
// IMS PERFORMANCE SCENARIOS — Main Entry Point
//
// Usage:
//   ./perf_scenarios              # Run all 5 scenarios (buggy → fixed)
//   ./perf_scenarios --scenario 1 # Run only scenario 1 (buggy → fixed)
//   ./perf_scenarios --scenario 1 --buggy  # Run ONLY the buggy version
//   ./perf_scenarios --scenario 1 --fixed  # Run ONLY the fixed version
//   ./perf_scenarios --list       # List all scenarios
//
// Each scenario runs BUGGY first, then FIXED, printing metrics.
// ============================================================

#include <iostream>
#include <string>
#include <cstring>
#include "perf_scenarios.h"

using namespace PerfScenarios;

void printUsage(const char* prog) {
    std::cout << "IMS Performance Scenarios — 5 Classic Interview Problems\n"
              << "\n"
              << "Usage: " << prog << " [options]\n"
              << "\n"
              << "Options:\n"
              << "  --scenario N    Run only scenario N (1-5)\n"
              << "  --list          List all scenarios with descriptions\n"
              << "  --help          Show this help\n"
              << "\n"
              << "Scenarios:\n"
              << "  1. CPU 100% — Busy-wait polling vs condition variable\n"
              << "  2. Lock Contention — Global mutex vs sharded by Call-ID\n"
              << "  3. Memory Leak — Raw pointers vs RAII (unique_ptr)\n"
              << "  4. Thread Starvation — Blocking on hot path vs async pool\n"
              << "  5. Inefficient Data Path — O(n) lookup vs hash-indexed\n"
              << "\n"
              << "Environment Variables:\n"
              << "  SCENARIO_VERBOSE=1    Print detailed per-scenario output\n"
              << std::flush;
}

void listScenarios() {
    std::cout << "\n"
              << "╔══════════════════════════════════════════════════════════════╗\n"
              << "║  IMS PERFORMANCE SCENARIOS — Interview Prep                  ║\n"
              << "╠══════════════════════════════════════════════════════════════╣\n"
              << "║  1. CPU 100% — Busy Wait vs Event-Driven                     ║\n"
              << "║     IMS Context: P-CSCF message queue polling                ║\n"
              << "║     Bug:     while(true) { if(queue.empty()) continue; }     ║\n"
              << "║     Fix:     condition_variable::wait()                      ║\n"
              << "║     Diagnose: top -H -p PID, perf top                       ║\n"
              << "╠══════════════════════════════════════════════════════════════╣\n"
              << "║  2. Lock Contention — Global Mutex vs Sharding               ║\n"
              << "║     IMS Context: Session table protected by one mutex        ║\n"
              << "║     Bug:     All threads contend on single global lock       ║\n"
              << "║     Fix:     Shard by hash(Call-ID) → 16 independent locks   ║\n"
              << "║     Diagnose: pidstat -t, perf lock                         ║\n"
              << "╠══════════════════════════════════════════════════════════════╣\n"
              << "║  3. Memory Leak — Raw Pointers vs RAII                       ║\n"
              << "║     IMS Context: SIP dialog contexts on error paths          ║\n"
              << "║     Bug:     new/delete with leak on exception path          ║\n"
              << "║     Fix:     unique_ptr + bounded queue with backpressure    ║\n"
              << "║     Diagnose: top/pidstat -r (RSS growth), valgrind         ║\n"
              << "╠══════════════════════════════════════════════════════════════╣\n"
              << "║  4. Thread Pool Starvation — Blocking on Hot Path            ║\n"
              << "╠══════════════════════════════════════════════════════════════╣\n"
              << "║     IMS Context: P-CSCF event loop doing blocking I/O       ║\n"
              << "║     Bug:     2 workers + blocking Diameter Rx calls         ║\n"
              << "║     Fix:     Pool sized to cores + offload blocking work    ║\n"
              << "║     Diagnose: thread states, latency percentiles            ║\n"
              << "╠══════════════════════════════════════════════════════════════╣\n"
              << "║  5. Inefficient Data Path — O(n) vs O(1) + Zero-Copy         ║\n"
              << "║     IMS Context: Session lookup + SIP message parsing        ║\n"
              << "║     Bug:     Vector scan for Call-ID + per-message copies    ║\n"
              << "║     Fix:     unordered_map + string_view (parse in place)    ║\n"
              << "║     Diagnose: perf record -g + flamegraph                   ║\n"
              << "╚══════════════════════════════════════════════════════════════╝\n"
              << std::flush;
}

int main(int argc, char* argv[]) {
    // Check for verbose mode
    bool verbose = (std::getenv("SCENARIO_VERBOSE") != nullptr);

    // Parse arguments
    int  target_scenario = 0;    // 0 = run all
    bool only_buggy      = false;
    bool only_fixed      = false;

    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
            printUsage(argv[0]);
            return 0;
        }
        if (std::strcmp(argv[i], "--list") == 0 || std::strcmp(argv[i], "-l") == 0) {
            listScenarios();
            return 0;
        }
        if (std::strcmp(argv[i], "--scenario") == 0 && i + 1 < argc) {
            target_scenario = std::atoi(argv[++i]);
            if (target_scenario < 1 || target_scenario > 5) {
                std::cerr << "Invalid scenario number: " << target_scenario << "\n";
                return 1;
            }
        }
        if (std::strcmp(argv[i], "--buggy") == 0) only_buggy = true;
        if (std::strcmp(argv[i], "--fixed") == 0) only_fixed = true;
    }

    // Pass mode into runner
    auto& runner = ScenarioRunner::instance();
    if (only_buggy) runner.setMode(false, true);   // buggy only
    if (only_fixed) runner.setMode(true, false);   // fixed only

    std::cout << "\n";
    std::cout << "╔══════════════════════════════════════════════════════════════╗\n";
    std::cout << "║  IMS / VoLTE PERFORMANCE SCENARIOS — Interview Prep          ║\n";
    std::cout << "║  5 classic problems in telecom systems (IMS/SIP/4G)          ║\n";
    std::cout << "╚══════════════════════════════════════════════════════════════╝\n";
    std::cout << std::flush;

    if (target_scenario > 0) {
        switch (target_scenario) {
            case 1: runner.runScenario1(); break;
            case 2: runner.runScenario2(); break;
            case 3: runner.runScenario3(); break;
            case 4: runner.runScenario4(); break;
            case 5: runner.runScenario5(); break;
        }
    } else {
        runner.runAll();
    }

    std::cout << "\n=== Performance scenarios complete ===\n";
    std::cout << "See docs/INTERVIEW_PERF_SCENARIOS.md for interview talking points.\n";

    return 0;
}