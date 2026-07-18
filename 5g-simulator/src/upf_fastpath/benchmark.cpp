// =============================================================================
// benchmark.cpp — Automated UPF forwarding path throughput/latency benchmark
//
// Usage:
//   upf_benchmark [--duration N] [--ues M] [--batch-size B] [--workers W]
//
// Default: 5 seconds per mode, 100 UEs, burst-size 32, single worker.
//
// Algorithm:
//   1. Build PFCP rule table with M UEs
//   2. Pre-generate 2M packets (all matching PDRs — measuring max throughput)
//   3. Run socket path: producer feeds all packets, wait until all processed
//   4. Run DPDK path: same
//   5. Print comparison table
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <random>
#include <string>
#include "packet.h"
#include "pfcp_rules.h"
#include "socket_path.h"
#include "dpdk_path.h"
#include "stats.h"

using namespace upf;
using namespace std::chrono;
using namespace std::chrono_literals;

// =============================================================================
// Pre-generate packets: all TEIDs/UE-IPs drawn from the rule table so every
// packet matches a PDR (no artificial drops from missing rules).
// Half UL (by TEID), half DL (by UE IP).
// =============================================================================
static std::vector<Packet> generate_packets(uint32_t num_ues, size_t total) {
    std::vector<Packet> pkts;
    pkts.reserve(total);

    const uint32_t base_ue_ip = (10u << 24) | (45u << 16) | (0u << 8) | 1u;
    const uint32_t base_teid  = 1000u;

    std::mt19937 rng(42);
    std::uniform_int_distribution<uint32_t> ue_dist(0, num_ues - 1);

    for (size_t i = 0; i < total; ++i) {
        uint32_t ue_idx = ue_dist(rng);
        Direction dir   = (i % 2 == 0) ? Direction::UL : Direction::DL;

        Packet p;
        p.seq        = (uint32_t)(i + 1);
        p.dir        = dir;
        p.teid       = base_teid + ue_idx;
        p.ue_ip      = base_ue_ip + ue_idx;
        p.dscp       = (ue_idx % 3 == 0) ? 46 : (ue_idx % 3 == 1) ? 34 : 0;
        p.payload_len = (dir == Direction::UL) ? 200 : 1400;
        p.qfi        = (ue_idx % 3 == 0) ? 1 : (ue_idx % 3 == 1) ? 2 : 9;
        // Set arrival_ns at generation time (not at send time)
        // This measures end-to-end latency from "packet ready" to "forwarded".
        // For throughput measurement, we reset arrival_ns at send time below.
        p.arrival_ns = Packet::now_ns();
        pkts.push_back(p);
    }
    return pkts;
}

// =============================================================================
// Wait for path to finish processing all packets (with 60s timeout)
// =============================================================================
static bool wait_until_done(const PathStats& stats, uint64_t target, int timeout_s = 60) {
    auto deadline = steady_clock::now() + seconds(timeout_s);
    while (steady_clock::now() < deadline) {
        uint64_t done = stats.pkts_forwarded.load(std::memory_order_relaxed)
                      + stats.pkts_dropped.load(std::memory_order_relaxed);
        if (done >= target) return true;
        std::this_thread::sleep_for(1ms);
    }
    return false;  // timeout
}

// =============================================================================
// Run socket path benchmark
// =============================================================================
static double run_socket_bench(const RuleTable& rt,
                                const std::vector<Packet>& pkts,
                                PathStats& out_stats) {
    printf("  [Socket] Starting...\n");
    fflush(stdout);

    SocketPath sp(rt);
    sp.start();

    auto t0 = high_resolution_clock::now();

    // Producer thread: feed all packets as fast as possible
    std::thread producer([&] {
        for (const auto& pkt : pkts) {
            Packet p = pkt;
            p.arrival_ns = Packet::now_ns();  // timestamp at send time

            // Spin-retry on back-pressure (queue full) rather than dropping
            while (!sp.enqueue(p)) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();

    bool ok = wait_until_done(sp.stats(), pkts.size());
    auto t1 = high_resolution_clock::now();

    sp.stop();

    if (!ok) {
        printf("  [Socket] WARNING: timeout waiting for completion\n");
    }

    // Copy stats out before destruction
    out_stats.pkts_rx.store(sp.stats().pkts_rx.load());
    out_stats.pkts_forwarded.store(sp.stats().pkts_forwarded.load());
    out_stats.pkts_dropped.store(sp.stats().pkts_dropped.load());
    out_stats.pkts_no_pdr.store(sp.stats().pkts_no_pdr.load());
    out_stats.latencies = sp.stats().latencies;
    out_stats.sample_idx.store(sp.stats().sample_idx.load());

    double elapsed = duration<double>(t1 - t0).count();
    printf("  [Socket] Done: %lu packets in %.2fs\n",
           (unsigned long)pkts.size(), elapsed);
    return elapsed;
}

// =============================================================================
// Run DPDK path benchmark
// =============================================================================
static double run_dpdk_bench(const RuleTable& rt,
                              const std::vector<Packet>& pkts,
                              PathStats& out_stats) {
    printf("  [DPDK ] Starting...\n");
    fflush(stdout);

    DpdkPath dp(rt);
    dp.start();

    auto t0 = high_resolution_clock::now();

    std::thread producer([&] {
        for (const auto& pkt : pkts) {
            Packet p = pkt;
            p.arrival_ns = Packet::now_ns();

            while (!dp.rx_enqueue(p)) {
                std::this_thread::yield();
            }
        }
    });

    producer.join();

    bool ok = wait_until_done(dp.stats(), pkts.size());
    auto t1 = high_resolution_clock::now();

    dp.stop();

    if (!ok) {
        printf("  [DPDK ] WARNING: timeout waiting for completion\n");
    }

    out_stats.pkts_rx.store(dp.stats().pkts_rx.load());
    out_stats.pkts_forwarded.store(dp.stats().pkts_forwarded.load());
    out_stats.pkts_dropped.store(dp.stats().pkts_dropped.load());
    out_stats.pkts_no_pdr.store(dp.stats().pkts_no_pdr.load());
    out_stats.latencies = dp.stats().latencies;
    out_stats.sample_idx.store(dp.stats().sample_idx.load());

    double elapsed = duration<double>(t1 - t0).count();
    printf("  [DPDK ] Done: %lu packets in %.2fs\n",
           (unsigned long)pkts.size(), elapsed);
    return elapsed;
}

// =============================================================================
// Main
// =============================================================================
int main(int argc, char* argv[]) {
    // Parse args
    int    duration_s  = 5;   // informational only (we run 2M packets)
    uint32_t num_ues   = 100;
    int    batch_size  = 32;
    int    num_workers = 1;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--duration") == 0 && i+1 < argc)   duration_s  = atoi(argv[++i]);
        else if (strcmp(argv[i], "--ues") == 0 && i+1 < argc)   num_ues     = (uint32_t)atoi(argv[++i]);
        else if (strcmp(argv[i], "--batch-size") == 0 && i+1 < argc) batch_size = atoi(argv[++i]);
        else if (strcmp(argv[i], "--workers") == 0 && i+1 < argc)    num_workers = atoi(argv[++i]);
    }

    printf("\n");
    printf("================================================================\n");
    printf("  UPF FAST-PATH BENCHMARK\n");
    printf("================================================================\n");
    printf("  UEs        : %u\n", num_ues);
    printf("  Packets    : 2,000,000 (2M pre-generated, all matching PDRs)\n");
    printf("  Batch size : %d (DPDK burst size)\n", batch_size);
    printf("  Workers    : %d (single lcore implemented)\n", num_workers);
    printf("  Duration   : %d s (informational; we run until all pkts done)\n", duration_s);
    printf("================================================================\n");
    printf("\n");

    // Build rules
    printf("[1/4] Building PFCP rule table for %u UEs...\n", num_ues);
    RuleTable rt = build_demo_rules(num_ues);
    printf("      PDRs: %zu, FARs: %zu, QERs: %zu\n",
           rt.pdrs_.size(), rt.fars_.size(), rt.qers_.size());
    printf("      Lookup indexes: %zu teid->pdr, %zu ueip->pdr\n",
           rt.teid_to_pdr_.size(), rt.ueip_to_pdr_.size());
    printf("\n");

    // Pre-generate packets
    printf("[2/4] Pre-generating 2,000,000 packets...\n");
    size_t total_pkts = 2'000'000;
    auto pkts = generate_packets(num_ues, total_pkts);
    printf("      Generated %zu packets (50%% UL by TEID, 50%% DL by UE-IP)\n", pkts.size());
    printf("\n");

    // Run socket bench
    printf("[3/4] Running socket-path benchmark...\n");
    PathStats socket_stats;
    double socket_dur = run_socket_bench(rt, pkts, socket_stats);
    printf("\n");

    // Run DPDK bench
    printf("[4/4] Running DPDK-path benchmark...\n");
    PathStats dpdk_stats;
    double dpdk_dur = run_dpdk_bench(rt, pkts, dpdk_stats);
    printf("\n");

    // Results table
    print_comparison_table("Socket", socket_stats, socket_dur,
                           "DPDK",   dpdk_stats,   dpdk_dur);

    // ── Production context ───────────────────────────────────────────────
    printf("\n");
    printf("================================================================\n");
    printf("  WHAT THIS MEANS IN PRODUCTION\n");
    printf("================================================================\n");
    printf("\n");
    printf("  This simulator models the ARCHITECTURE, not the absolute\n");
    printf("  performance numbers. Real DPDK UPF gains come from:\n");
    printf("\n");
    printf("  [rte_eth_rx_burst] Instead of our SPSCRing, the NIC PMD fills\n");
    printf("    mbufs directly via DMA into pre-registered hugepage memory.\n");
    printf("    No copy from kernel space. The lcore sees packets arrive ~100ns\n");
    printf("    after they land on the wire.\n");
    printf("\n");
    printf("  [Hugepages] 2MB pages (vs default 4KB) reduce TLB misses by 512x.\n");
    printf("    At 10Mpps, a 4KB-page UPF would need ~37,500 TLB entries/sec\n");
    printf("    for packet data alone. 2MB hugepages use ~73 entries/sec.\n");
    printf("    Configure: echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages\n");
    printf("\n");
    printf("  [VFIO/UIO] The NIC PCI device is remapped to userspace. No kernel\n");
    printf("    driver, no interrupt handler, no sk_buff. The application owns\n");
    printf("    the NIC completely. Requires: modprobe vfio-pci + dpdk-devbind.\n");
    printf("\n");
    printf("  [PMD] Poll Mode Driver replaces interrupt-driven kernel driver.\n");
    printf("    Examples: mlx5_pmd (Mellanox), i40e (Intel XL710), dpaa2 (NXP).\n");
    printf("    Each PMD is optimized for its NIC's DMA descriptor format.\n");
    printf("\n");
    printf("  [NUMA pinning] In multi-socket servers, DPDK pins each lcore to\n");
    printf("    a NUMA node co-located with the NIC. Cross-NUMA memory access\n");
    printf("    adds ~100ns latency and halves effective memory bandwidth.\n");
    printf("    Use: rte_socket_id(), rte_lcore_to_socket_id().\n");
    printf("\n");
    printf("  [Production UPF throughput]\n");
    printf("    OAI-UPF (VPP-based)    : 10-40 Mpps on 1 core\n");
    printf("    free5GC UPF (kernel)   : 200K-1 Mpps on 1 core\n");
    printf("    DPDK UPF (custom PMD)  : 40-100 Mpps on 1 core (NIC limited)\n");
    printf("    Our simulator          : Models the gap, not the absolute numbers\n");
    printf("\n");
    printf("  [PFCP runtime updates] The SMF sends PFCP Session Modification\n");
    printf("    Requests (e.g., on handover, QoS change, UE idle). The UPF\n");
    printf("    must update PDR/FAR tables without stalling the lcore. Real\n");
    printf("    UPFs use RCU (Read-Copy-Update) or per-core rule caches with\n");
    printf("    a quiescent state mechanism.\n");
    printf("\n");
    printf("  [Observability] Production UPFs export:\n");
    printf("    - PFCP Session stats per UE (octets, packets per QFI)\n");
    printf("    - Prometheus metrics: upf_forwarded_total, upf_latency_ns_p99\n");
    printf("    - DPDK ethdev stats: rx_good_packets, tx_good_packets, rx_missed\n");
    printf("================================================================\n");
    printf("\n");

    return 0;
}
