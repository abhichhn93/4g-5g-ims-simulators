#pragma once
// =============================================================================
// dpdk_path.h — DPDK poll-mode UPF path (simulation)
//
// This models the key architectural differences of a DPDK-based UPF:
//
//   1. NO KERNEL INVOLVEMENT: The NIC is mapped directly to userspace via
//      VFIO (Virtual Function I/O) or UIO (Userspace I/O). The kernel's
//      network stack is completely bypassed.
//
//   2. POLL MODE DRIVER (PMD): Instead of hardware interrupts, the lcore
//      (logical core = DPDK's name for a pinned thread on one CPU core)
//      busy-polls the NIC RX descriptor ring continuously.
//      rte_eth_rx_burst(port, queue, mbufs, BURST_SIZE) — returns immediately
//      with 0-BURST_SIZE packets. Never blocks, never sleeps.
//
//   3. BURST PROCESSING: The PMD fills a burst of up to 32 mbufs in one call.
//      The lcore then processes all 32 packets before polling again. This
//      amortizes per-batch overhead (now_ns(), branch prediction, cache warm-up)
//      across 32 packets instead of paying it per-packet.
//
//   4. PRE-ALLOCATED MBUFS: No kmalloc on the hot path. The mbuf pool is
//      pre-populated at startup from hugepages. alloc = ring pop (~5ns).
//
//   5. ZERO-COPY (in real DPDK): NIC DMA writes directly into the pre-allocated
//      mbuf data region. No memcpy between NIC and application.
//      (We simulate this by copying once into the mbuf at rx_enqueue time.)
//
// TRADE-OFF: The lcore runs at 100% CPU even when traffic is 0. This is
// intentional in 5G UPF deployments where latency SLA for voice (5QI=1)
// demands <1ms UL latency — you can't afford even a 1ms scheduler wakeup.
//
// WHAT WE SIMULATE:
//   rx_ring_:   simulates the NIC RX descriptor ring (hardware → software)
//   mbuf_pool_: simulates rte_mempool backed by hugepages
//   lcore_poll_loop(): simulates the DPDK lcore poll loop
//   pop_burst():       simulates rte_eth_rx_burst()
// =============================================================================

#include <atomic>
#include <thread>
#include <cstring>
#include "packet.h"
#include "pfcp_rules.h"
#include "ring_buffer.h"
#include "mbuf_pool.h"
#include "stats.h"

namespace upf {

class DpdkPath {
public:
    // ── Configuration ─────────────────────────────────────────────────────
    // BURST_SIZE: number of packets processed per poll iteration.
    // Real DPDK default: 32. Can be 1-64. Larger bursts improve throughput
    // but increase worst-case latency (last packet in burst waits for burst
    // to fill before processing begins).
    static constexpr size_t BURST_SIZE = 32;

    // RING_SIZE: must be power of 2. Represents the NIC RX descriptor ring.
    // Real NIC: 512-4096 descriptors. Each descriptor points to a pre-mapped
    // mbuf DMA address. We use 4096 for headroom in our simulation.
    static constexpr size_t RING_SIZE  = 4096;

    explicit DpdkPath(const RuleTable& rules)
        : rules_(rules), mbuf_pool_(65536), running_(false) {}

    ~DpdkPath() {
        if (running_.load()) stop();
    }

    // ── start ────────────────────────────────────────────────────────────
    // In real DPDK: rte_eal_init() (DPDK EAL = Environment Abstraction Layer)
    // followed by rte_eth_dev_configure(), rte_eth_dev_start().
    // Our simulation just launches a C++ thread pinned to poll the ring.
    void start() {
        running_.store(true, std::memory_order_seq_cst);
        worker_ = std::thread([this]{ lcore_poll_loop(); });
    }

    // ── stop ─────────────────────────────────────────────────────────────
    // In real DPDK: rte_eth_dev_stop(), rte_eth_dev_close(), rte_eal_cleanup().
    void stop() {
        running_.store(false, std::memory_order_seq_cst);
        if (worker_.joinable()) worker_.join();
    }

    // ── rx_enqueue ───────────────────────────────────────────────────────
    // Simulates the packet arriving at the NIC and being DMA'd into an mbuf.
    // In real DPDK: the PMD handles this transparently — by the time the
    // lcore calls rte_eth_rx_burst(), the mbuf is already filled.
    // We use a lock-free SPSCRing to hand the mbuf pointer to the lcore.
    bool rx_enqueue(const Packet& pkt) {
        stats_.pkts_rx.fetch_add(1, std::memory_order_relaxed);

        // Alloc mbuf from pool (simulates NIC DMA filling a pre-mapped mbuf)
        Mbuf* m = mbuf_pool_.alloc();
        if (!m) {
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Copy packet metadata into mbuf (in real DPDK: zero-copy, NIC DMA)
        m->pkt = pkt;
        m->in_use = true;

        // Push mbuf pointer to RX ring (lock-free, ~5-10ns)
        if (!rx_ring_.push(m)) {
            mbuf_pool_.free_mbuf(m);
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    PathStats& stats() { return stats_; }
    const PathStats& stats() const { return stats_; }

private:
    const RuleTable&           rules_;
    SPSCRing<Mbuf*, RING_SIZE> rx_ring_;
    MbufPool                   mbuf_pool_;
    std::atomic<bool>          running_;
    std::thread                worker_;
    PathStats                  stats_;

    // ── lcore_poll_loop ─────────────────────────────────────────────────
    // This is the DPDK "lcore function" — the tight poll loop that runs
    // on a dedicated CPU core. In real DPDK:
    //   rte_eal_remote_launch(lcore_fn, NULL, lcore_id);
    //
    // KEY PROPERTIES:
    //   - Never calls sleep(), mutex_lock(), or any blocking syscall
    //   - If no packets available: immediately loops back (busy-wait)
    //   - 100% CPU usage is expected and acceptable in production UPF
    //   - Packet-to-packet latency is bounded only by CPU clock (~10-50ns)
    //     not by OS scheduler (~100μs-5ms)
    void lcore_poll_loop() {
        Mbuf* burst[BURST_SIZE];

        while (running_.load(std::memory_order_relaxed)) {
            // rte_eth_rx_burst() equivalent: pop up to BURST_SIZE mbufs
            size_t n = rx_ring_.pop_burst(burst, BURST_SIZE);
            if (n == 0) {
                // Ring empty: immediately poll again (busy-wait).
                // NO sleep, NO yield, NO condition_variable.
                // This is what burns the CPU core but eliminates scheduler jitter.
                continue;
            }
            process_burst(burst, n);
        }

        // Drain remaining packets
        while (true) {
            size_t n = rx_ring_.pop_burst(burst, BURST_SIZE);
            if (n == 0) break;
            process_burst(burst, n);
        }
    }

    // ── process_burst ────────────────────────────────────────────────────
    // Process a burst of mbufs. This is where DPDK's efficiency comes from:
    //
    // 1. SINGLE now_ns() CALL FOR THE ENTIRE BURST:
    //    Reading the TSC (Time Stamp Counter) via rdtsc or
    //    clock_gettime() costs ~20-40ns. At 32 packets/burst, paying this
    //    once vs 32 times saves (31 × 30ns) = ~930ns per burst.
    //    In production DPDK: rte_rdtsc() which compiles to a single rdtsc
    //    instruction. Real hardware timestamping is even cheaper.
    //
    // 2. CACHE-WARM PDR LOOKUPS:
    //    If multiple packets in a burst belong to the same UE (common in
    //    voice streams), the second lookup hits L1 cache instead of L2/L3.
    //    The hash table entry for that TEID is still warm from the previous
    //    packet's lookup.
    //
    // 3. BRANCH PREDICTOR LEARNS THE LOOP:
    //    The CPU branch predictor sees the same loop pattern for all packets
    //    in the burst and reaches steady-state prediction by packet 3-4.
    //    Per-packet loop startup cost is amortized.
    void process_burst(Mbuf** burst, size_t n) {
        // ONE timestamp for the whole burst — key DPDK optimization
        uint64_t now = Packet::now_ns();

        for (size_t i = 0; i < n; ++i) {
            Mbuf* m = burst[i];
            Packet& pkt = m->pkt;

            // PDR lookup
            const PDR* pdr = rules_.match_pdr(pkt);
            if (!pdr) {
                stats_.pkts_no_pdr.fetch_add(1, std::memory_order_relaxed);
                stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                mbuf_pool_.free_mbuf(m);
                continue;
            }
            pkt.pdr_id = pdr->pdr_id;
            pkt.far_id = pdr->far_id;
            pkt.classified = true;

            // QER lookup
            const QER* qer = rules_.get_qer(pdr->qer_id);
            (void)qer;

            // FAR action
            const FAR* far = rules_.get_far(pdr->far_id);
            if (!far || far->action == FarAction::DROP) {
                pkt.dropped = true;
                stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                mbuf_pool_.free_mbuf(m);
                continue;
            }

            // FORWARD
            stats_.pkts_forwarded.fetch_add(1, std::memory_order_relaxed);

            // Record latency using the burst-level timestamp (not per-packet)
            // In real DPDK: pkt->timestamp (hardware NIC timestamp) vs rdtsc now
            stats_.record_latency(pkt.arrival_ns, now);

            // Free mbuf back to pool (in real DPDK: rte_pktmbuf_free() or
            // hand to TX burst buffer for rte_eth_tx_burst())
            mbuf_pool_.free_mbuf(m);
        }
    }
};

} // namespace upf
