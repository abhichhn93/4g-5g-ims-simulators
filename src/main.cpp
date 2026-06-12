#include <iostream>
#include <string>
#include <thread>
#include <atomic>
#include <csignal>
#include <sstream>
#include <chrono>
#include <memory>

#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/pcap_writer.h"
#include "common/thread_pool.h"
#include "common/metrics.h"
#include "enb/enb_node.h"
#include "mme/mme_node.h"
#include "hss/hss_node.h"
#include "sgw/sgw_node.h"
#include "pgw/pgw_node.h"
#include "pcrf/pcrf_node.h"

// ============================================================
// THREADING MODEL — Phase 4 (complete)
//
//   main thread ─── CLI + thread pool (BULK)
//        │
//   ┌────┼────────────────────────────────────────────┐
//  hss   enb     mme         sgw     pgw     pcrf
//  3868  38412 2125+38412+3868 2123   2124    3869
//  TCP   TCP   UDP+TCP+TCP   UDP     UDP+TCP  TCP
//
// CONCURRENCY PRIMITIVES (all 4 phases):
//
//  Phase 1: mutex+cv          CLI→eNB command queue (producer-consumer)
//  Phase 2: atomic stop       SIGINT/SIGTERM graceful shutdown
//           cv handoff        HSS→MME auth vector delivery
//           write mutex       commandLoop+receiveLoop share TCP socket
//  Phase 3: 64×shared_mutex   sharded UE context store (reduce contention)
//           atomic fetch_add  lock-free IP and TEID allocation (P-GW)
//           shared_ptr        UeContext shared across multiple threads
//  Phase 4: thread pool       BULK N concurrent CR submissions
//           Flyweight         shared SubscriberProfile (immutable, shared_ptr)
//           Metrics           per-attach latency, P95/P99 throughput
//
// DESIGN PATTERNS DEMONSTRATED:
//   Producer-Consumer:  CLI→eNB queue, HSS→MME auth handoff
//   RAII:               Socket destructor closes fd; shared_ptr frees UeContext
//   State machine:      EMM states per UE (DEREGISTERED→REGISTERED)
//   Flyweight:          SubscriberProfile shared across UEs
//   Thread Pool:        8 workers for BULK N command
//   Observer (future):  PCRF→P-GW RAR triggers dedicated bearer (Phase 5)
//   Sharding:           64-bucket UE store with per-bucket shared_mutex
//
// INTERVIEW Q: "Walk me through the concurrency in your MME simulator"
// ANSWER: "Each network node is a thread with its own receive loop.
//   I use condition_variable for two async handoffs: CLI→eNB commands
//   and HSS→MME auth vectors. A sharded UE store with shared_mutex gives
//   concurrent reads. Atomic counters for TEID/IP allocation.
//   BULK uses a thread pool to fan out CR submissions.
//   SIGINT sets an atomic stop flag checked by all loops — clean shutdown."
// ============================================================

static std::atomic<bool>* g_stop = nullptr;
static void sig_handler(int) { if(g_stop) g_stop->store(true); }

int main() {
    VLog::printStartupDiagram();
    PcapWriter::instance().open("mme_capture.pcap");

    std::atomic<bool> stop{false};
    std::atomic<bool> enb_ready{false}, hss_ready{false};
    std::atomic<bool> sgw_ready{false}, pgw_ready{false}, pcrf_ready{false};

    g_stop = &stop;
    std::signal(SIGINT,  sig_handler);
    std::signal(SIGTERM, sig_handler);

    // Shared metrics object (MME writes, main thread reads)
    auto metrics = std::make_shared<Metrics>();

    HssNode  hss (stop, hss_ready);
    SgwNode  sgw (stop, sgw_ready);
    PgwNode  pgw (stop, pgw_ready, pcrf_ready);
    PcrfNode pcrf(stop, pcrf_ready);
    EnbNode  enb (stop, enb_ready);
    MmeNode  mme (stop, enb_ready, hss_ready, sgw_ready, metrics);

    // Thread pool for BULK: 8 workers (= hardware_concurrency typical)
    // Created once at startup, reused for all BULK commands
    ThreadPool pool(8, "BULK");

    // Start order: servers first (they need to bind before clients connect)
    std::thread hss_th ([&]{ hss.run();  });
    std::thread pcrf_th([&]{ pcrf.run(); });
    std::thread sgw_th ([&]{ sgw.run();  });
    std::thread pgw_th ([&]{ pgw.run();  });
    std::thread enb_th ([&]{ enb.run();  });
    std::thread mme_th ([&]{ mme.run();  });

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    Logger::sys("──────────────────────────────────────────────────────────────────");
    std::string line;
    std::cout << "\nmme-sim> " << std::flush;

    while (!stop.load() && std::getline(std::cin, line)) {
        if (stop.load()) break;
        if (line.empty()) { std::cout << "mme-sim> " << std::flush; continue; }

        std::istringstream iss(line); std::string cmd; iss >> cmd;
        for(auto& c:cmd) c=char(std::toupper(unsigned(c)));

        if (cmd=="QUIT"||cmd=="EXIT") {
            Logger::sys("QUIT — shutting down...");
            break;
        }
        else if (cmd=="STATUS") {
            mme.printStatus();
        }
        else if (cmd=="CR") {
            int n=1; if(!(iss>>n)||n<1) n=1;
            if(n>100){Logger::sys("capped at 100");n=100;}
            Logger::sys("CR " + std::to_string(n) + " — starting attach flow(s)");
            enb.submitCommand("CR " + std::to_string(n));
        }
        else if (cmd=="BULK") {
            // ── BULK N: fan out N attach tasks across thread pool workers ──
            // Each worker submits one "CR 1" to eNB's command queue.
            // MME processes each sequentially (single-thread bottleneck shown by P99).
            // INTERVIEW: "This shows where the bottleneck is. P99 grows linearly
            //   because MME's handleInitialUEMsg blocks on HSS cv.wait per UE.
            //   Phase 5 fix: async auth — one condition_variable per UE, all in flight."
            int n=5; if(!(iss>>n)||n<1) n=5;
            if(n>100){Logger::sys("capped at 100 for demo");n=100;}

            Logger::sys("BULK " + std::to_string(n) + " — submitting to 8-worker thread pool");
            Logger::sys("THREAD POOL: 8 workers, shared task queue, mutex+cv wake pattern");
            Logger::sys("Each worker calls enb.submitCommand(CR 1) — then MME processes sequentially");

            metrics->startBulk(n);  // reset metrics for this batch

            for (int i = 0; i < n; ++i) {
                pool.submit([&enb]() {
                    // Worker thread executes this — submits one attach to eNB queue
                    enb.submitCommand("CR 1");
                });
            }

            Logger::sys("BULK: all " + std::to_string(n) + " tasks queued in pool");
            Logger::sys("BULK: waiting for all " + std::to_string(n) + " attaches to complete (30s timeout)...");

            bool all_done = metrics->waitForAll(30000);
            if (all_done) {
                Logger::sys("BULK: ✓ all " + std::to_string(n) + " UEs registered");
            } else {
                Logger::sys("BULK: timeout! only " + std::to_string(metrics->completedCount()) +
                            "/" + std::to_string(n) + " completed");
            }
            metrics->printReport();
        }
        else {
            Logger::sys("Unknown: '" + line + "'. Try: CR 1, BULK 5, STATUS, QUIT");
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        if (!stop.load()) std::cout << "\nmme-sim> " << std::flush;
    }

    Logger::sys("Stopping...");
    stop.store(true);
    enb.requestStop();

    Logger::sys("joining hss...");  hss_th.join();
    Logger::sys("joining pcrf..."); pcrf_th.join();
    Logger::sys("joining sgw...");  sgw_th.join();
    Logger::sys("joining pgw...");  pgw_th.join();
    Logger::sys("joining enb...");  enb_th.join();
    Logger::sys("joining mme...");  mme_th.join();
    Logger::sys("Done. Goodbye.");
    Logger::shutdown();
    return 0;
}
