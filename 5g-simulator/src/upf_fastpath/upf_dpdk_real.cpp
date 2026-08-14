// =============================================================================
// upf_dpdk_real.cpp — Real DPDK entrypoint for Oracle Cloud Linux
//
// This is the program that actually calls rte_eal_init(), sets up hugepages,
// configures the NIC, and launches the lcore poll loop using real DPDK functions.
//
// On Mac (simulation mode): prints a message and exits.
// On Oracle Cloud (ENABLE_DPDK=1): full real DPDK execution.
//
// BUILD:
//   Mac (simulation, no real DPDK):   make                → upf_demo, upf_benchmark
//   Oracle Cloud (real DPDK):         make ENABLE_DPDK=1  → also builds upf_real
//
// RUN ON ORACLE CLOUD:
//   Option A — virtual tap (no physical NIC needed, easiest):
//     sudo ./upf_real -l 0-1 -n 2 --vdev net_tap0,iface=tap0 -- --ues 100
//
//   Option B — real physical NIC (after dpdk-devbind.py):
//     sudo ./upf_real -l 0-3 -n 4 -a 0000:00:03.0 -- --ues 1000
//
// EAL ARGS (before --):
//   -l 0-1        which CPU cores to use (0=main, 1=lcore poll loop)
//   -n 2          memory channels (match your DDR channel count)
//   --vdev ...    virtual device (tap, ring, memif) — no real NIC needed
//   -a 0000:xx.x  allow/bind a real NIC by PCI address
//
// APP ARGS (after --):
//   --ues N       number of simulated UE sessions (PDR/FAR/QER rules)
//   --duration N  seconds to run before stopping
//   --port N      NIC port id (default 0)
//   --lcore N     which lcore to run the poll loop on (default 1)
//
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <string>
#include <atomic>

#include "dpdk_compat.h"   // real rte_* when ENABLE_DPDK=1, simulation otherwise
#include "pfcp_rules.h"
#include "stats.h"

// ─────────────────────────────────────────────────────────────────────────────
// REAL DPDK MODE — compiled when: make ENABLE_DPDK=1
// ─────────────────────────────────────────────────────────────────────────────
#ifdef ENABLE_DPDK

using namespace upf;

// Global stop flag — set by Ctrl-C handler
static std::atomic<bool> g_running{true};

static void sigint_handler(int) {
    g_running.store(false, std::memory_order_relaxed);
    printf("\n[main] Ctrl-C caught — signaling lcore to stop...\n");
}

// Parse app args (everything after --)
struct AppArgs {
    uint32_t num_ues  = 100;
    int      duration = 10;
    uint16_t port_id  = 0;
    unsigned lcore_id = 1;
};

static AppArgs parse_app_args(int argc, char** argv) {
    AppArgs a;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--ues") == 0 && i+1 < argc)
            a.num_ues = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--duration") == 0 && i+1 < argc)
            a.duration = atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i+1 < argc)
            a.port_id = (uint16_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--lcore") == 0 && i+1 < argc)
            a.lcore_id = (unsigned)atoi(argv[++i]);
    }
    return a;
}

// ─────────────────────────────────────────────────────────────────────────────
// MAIN — real DPDK startup sequence
// ─────────────────────────────────────────────────────────────────────────────
int main(int argc, char* argv[]) {
    signal(SIGINT, sigint_handler);

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║   UPF FAST-PATH — REAL DPDK MODE (Oracle Cloud)                ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    // ── STEP 1: rte_eal_init ─────────────────────────────────────────────────
    // This is the FIRST rte_* call any DPDK program must make.
    //
    // What it does:
    //   1. Parses EAL arguments from argv (-l, -n, --vdev, -a, etc.)
    //   2. Reserves hugepages from /mnt/huge (mounted by oracle_setup.sh)
    //   3. Discovers and initializes lcores (CPU cores)
    //   4. Initializes the memory subsystem (rte_malloc, rte_mempool)
    //   5. If --vdev net_tap0: creates a virtual tap network device
    //   6. If -a 0000:xx.x: binds and initializes the real NIC via VFIO
    //
    // Returns: number of EAL args consumed. Your app args start at argv[ret+1].
    // Exits on failure (calls rte_exit() which is like abort()).
    //
    printf("[STEP 1] rte_eal_init()...\n");
    int eal_argc = argc;
    char** eal_argv = argv;
    int ret = rte_eal_init(eal_argc, eal_argv);
    if (ret < 0) {
        fprintf(stderr, "[ERROR] rte_eal_init failed: %s\n", rte_strerror(rte_errno));
        return 1;
    }
    // Remaining args after EAL are our app args
    int app_argc = argc - ret;
    char** app_argv = argv + ret;
    AppArgs args = parse_app_args(app_argc, app_argv);

    printf("[STEP 1] EAL init OK. EAL consumed %d args, app has %d args.\n", ret, app_argc);
    printf("         Available lcores: ");
    unsigned lc;
    RTE_LCORE_FOREACH(lc) { printf("%u ", lc); }
    printf("\n");
    printf("         NUMA nodes: %u\n", rte_socket_count());
    printf("\n");

    // ── STEP 2: Create mbuf pool on hugepages ────────────────────────────────
    // rte_pktmbuf_pool_create allocates from the hugepages reserved by EAL.
    //
    // Parameters:
    //   name:       "n3_pool" — unique name (DPDK has a global pool registry)
    //   n:          65535 mbufs (must be power-of-2 minus 1 for rte_ring alignment)
    //   cache_size: 512 — per-lcore cache. alloc/free within cache = ~5 cycles.
    //               When cache misses: bulk transfer to/from global ring (~30 cycles).
    //   priv_size:  0 — no extra private data appended to each mbuf header
    //   data_room:  RTE_MBUF_DEFAULT_BUF_SIZE = 2176 bytes
    //               = 128 bytes headroom + 2048 bytes data region
    //               Headroom lets DL path prepend GTP-U/UDP/IP without memcpy
    //   socket_id:  NUMA node of the NIC. Mismatched NUMA → +100ns per mbuf access.
    //
    printf("[STEP 2] Creating mbuf pool on hugepages...\n");
    RealMbufPool n3_pool("n3_pool", args.port_id);  // hugepage-backed, NUMA-local
    printf("[STEP 2] Pool created: %u mbufs × %u bytes = ~%.0f MB on hugepages\n",
           RealMbufPool::NUM_MBUFS,
           RTE_MBUF_DEFAULT_BUF_SIZE,
           (double)RealMbufPool::NUM_MBUFS * RTE_MBUF_DEFAULT_BUF_SIZE / 1024 / 1024);
    printf("\n");

    // ── STEP 3: Configure NIC port ───────────────────────────────────────────
    // rte_eth_dev_configure: set RX/TX queue count and port-level options
    // rte_eth_rx_queue_setup: set up RX descriptor ring (1024 descriptors)
    //   Each descriptor is a (mbuf_pointer, done_flag) pair.
    //   NIC DMA fills these. lcore reads them via rte_eth_rx_burst().
    // rte_eth_tx_queue_setup: TX descriptor ring (lcore writes, NIC reads + sends)
    // rte_eth_dev_start: starts the NIC DMA engine. After this, packets flow.
    //
    // For --vdev net_tap0: the virtual tap driver emulates a real NIC.
    //   Traffic: send from your lcore → appears on the tap0 Linux interface.
    //   You can inject test packets: echo ... | socat - /dev/tap0
    //
    printf("[STEP 3] Configuring NIC port %u...\n", args.port_id);
    RealNicConfig::configure(args.port_id, n3_pool.pool());
    printf("[STEP 3] NIC configured: 1 RX queue × %u descriptors, 1 TX queue × %u descriptors\n",
           RealNicConfig::RX_DESC, RealNicConfig::TX_DESC);
    printf("\n");

    // ── STEP 4: Build PFCP rule table ────────────────────────────────────────
    // In production: SMF sends PFCP Session Establishment Requests (N4 interface).
    // Each request installs PDR + FAR + QER for one UE session.
    // Here we build demo rules for --ues N simulated UE sessions.
    //
    printf("[STEP 4] Building PFCP rule table for %u UEs...\n", args.num_ues);
    RuleTable rt = build_demo_rules(args.num_ues);
    printf("[STEP 4] Rules: %zu PDRs, %zu FARs, %zu QERs\n",
           rt.pdrs_.size(), rt.fars_.size(), rt.qers_.size());
    printf("         UL lookup: teid_to_pdr_ (%zu entries)\n", rt.teid_to_pdr_.size());
    printf("         DL lookup: ueip_to_pdr_ (%zu entries)\n", rt.ueip_to_pdr_.size());
    printf("\n");

    // ── STEP 5: Launch lcore poll loop ───────────────────────────────────────
    // rte_eal_remote_launch pins real_lcore_fn to a specific CPU core.
    // Under the hood: sched_setaffinity() isolates the thread to that core.
    // For best results: boot with isolcpus=1 so the OS never preempts this core.
    //
    // real_lcore_fn (defined in dpdk_compat.h) does:
    //   while (running) {
    //     n = rte_eth_rx_burst(port, queue, burst, 32);   // poll NIC
    //     parse TEID from raw bytes (rte_pktmbuf_mtod)
    //     match_pdr(teid) → get_far() → forward/drop
    //     rte_pktmbuf_free(m)  // return mbuf to per-lcore cache
    //   }
    //
    PathStats lcore_stats;
    RealLcoreArgs lcore_args{
        .port_id = args.port_id,
        .queue_id = 0,
        .rules = &rt,
        .stats = &lcore_stats,
        .running = &g_running,
    };

    printf("[STEP 5] Launching lcore %u on port %u queue 0...\n",
           args.lcore_id, args.port_id);
    printf("         lcore runs: rte_eth_rx_burst() → TEID parse → PDR/FAR → rte_pktmbuf_free()\n");
    printf("         CPU usage: 100%% on core %u (busy-polling, no sleep)\n", args.lcore_id);
    printf("\n");

    launch_real_lcore(args.lcore_id, &lcore_args);

    // ── STEP 6: Main loop — print stats periodically ─────────────────────────
    printf("[RUNNING] Lcore %u is polling. Ctrl-C to stop. Duration: %ds\n\n",
           args.lcore_id, args.duration);

    printf("  %-12s %-12s %-12s %-12s %-12s\n",
           "Elapsed(s)", "RX", "Forwarded", "Dropped", "No-PDR");
    printf("  %-12s %-12s %-12s %-12s %-12s\n",
           "----------", "----------", "----------", "----------", "----------");

    auto t_start = std::chrono::steady_clock::now();
    int elapsed = 0;

    while (g_running.load() && elapsed < args.duration) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        elapsed++;

        uint64_t rx   = lcore_stats.pkts_rx.load(std::memory_order_relaxed);
        uint64_t fwd  = lcore_stats.pkts_forwarded.load(std::memory_order_relaxed);
        uint64_t drop = lcore_stats.pkts_dropped.load(std::memory_order_relaxed);
        uint64_t nopdr= lcore_stats.pkts_no_pdr.load(std::memory_order_relaxed);

        printf("  %-12d %-12lu %-12lu %-12lu %-12lu\n",
               elapsed,
               (unsigned long)rx,
               (unsigned long)fwd,
               (unsigned long)drop,
               (unsigned long)nopdr);
        fflush(stdout);
    }

    // ── STEP 7: Stop lcore ───────────────────────────────────────────────────
    g_running.store(false, std::memory_order_relaxed);
    wait_all_lcores();  // rte_eal_mp_wait_lcore() — waits for all remote lcores to exit
    printf("\n[STEP 7] Lcore stopped.\n\n");

    // ── STEP 8: Print final stats ─────────────────────────────────────────────
    auto perc = lcore_stats.compute_percentiles();
    auto t_end = std::chrono::steady_clock::now();
    double dur_s = std::chrono::duration<double>(t_end - t_start).count();

    uint64_t total = lcore_stats.pkts_forwarded.load() + lcore_stats.pkts_dropped.load();
    double mpps = (dur_s > 0) ? (double)total / dur_s / 1e6 : 0.0;

    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║   RESULTS — REAL DPDK                                          ║\n");
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  Duration          : %.2f s                                    \n", dur_s);
    printf("║  Packets RX        : %lu                                       \n",
           (unsigned long)lcore_stats.pkts_rx.load());
    printf("║  Packets forwarded : %lu                                       \n",
           (unsigned long)lcore_stats.pkts_forwarded.load());
    printf("║  Packets dropped   : %lu                                       \n",
           (unsigned long)lcore_stats.pkts_dropped.load());
    printf("║  No-PDR drops      : %lu                                       \n",
           (unsigned long)lcore_stats.pkts_no_pdr.load());
    printf("║  Throughput        : %.3f Mpps                                 \n", mpps);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  Latency avg       : %lu ns                                    \n",
           (unsigned long)perc.avg_ns);
    printf("║  Latency p50       : %lu ns                                    \n",
           (unsigned long)perc.p50_ns);
    printf("║  Latency p95       : %lu ns                                    \n",
           (unsigned long)perc.p95_ns);
    printf("║  Latency p99       : %lu ns                                    \n",
           (unsigned long)perc.p99_ns);
    printf("╠══════════════════════════════════════════════════════════════════╣\n");
    printf("║  NIC ethdev stats:\n");
    struct rte_eth_stats eth_stats{};
    rte_eth_stats_get(args.port_id, &eth_stats);
    printf("║    NIC rx_packets  : %lu\n", (unsigned long)eth_stats.ipackets);
    printf("║    NIC tx_packets  : %lu\n", (unsigned long)eth_stats.opackets);
    printf("║    NIC rx_missed   : %lu  (ring full → dropped at NIC)\n",
           (unsigned long)eth_stats.imissed);
    printf("║    NIC rx_errors   : %lu\n", (unsigned long)eth_stats.ierrors);
    printf("╚══════════════════════════════════════════════════════════════════╝\n");

    // ── STEP 9: Cleanup ───────────────────────────────────────────────────────
    rte_eth_dev_stop(args.port_id);
    rte_eth_dev_close(args.port_id);
    rte_eal_cleanup();  // free hugepages, close VFIO

    printf("\n[DONE] Hugepages released. VFIO closed.\n\n");
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// SIMULATION MODE (Mac, ENABLE_DPDK not set)
// ─────────────────────────────────────────────────────────────────────────────
#else  // !ENABLE_DPDK

int main() {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║   upf_real — Real DPDK Mode                                    ║\n");
    printf("║                                                                  ║\n");
    printf("║   This binary requires real DPDK (Oracle Cloud Linux).           ║\n");
    printf("║   You are running in simulation mode (Mac / no DPDK installed). ║\n");
    printf("║                                                                  ║\n");
    printf("║   To run on your Mac (simulation):                               ║\n");
    printf("║     ./upf_demo --dpdk                                            ║\n");
    printf("║     ./upf_benchmark --ues 100                                    ║\n");
    printf("║                                                                  ║\n");
    printf("║   To run with real DPDK on Oracle Cloud:                         ║\n");
    printf("║     1. SSH into Oracle Cloud A1 VM                               ║\n");
    printf("║     2. sudo ./oracle_setup.sh                                    ║\n");
    printf("║     3. make ENABLE_DPDK=1                                        ║\n");
    printf("║     4. sudo ./upf_real -l 0-1 -n 2 --vdev net_tap0 -- --ues 100║\n");
    printf("║                                                                  ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");
    return 0;
}

#endif  // ENABLE_DPDK
