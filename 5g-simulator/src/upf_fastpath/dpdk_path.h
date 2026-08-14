#pragma once
// =============================================================================
// dpdk_path.h — DPDK poll-mode UPF path (simulation)
//
// ─────────────────────────────────────────────────────────────────────────────
// THE BIG PICTURE — where this file fits in the full DPDK pipeline:
//
//           WIRE  (physical ethernet cable)
//             |
//             v
//         +-------+
//         |  NIC  |   Your network interface card
//         +-------+
//             |
//             | DMA  ← NIC copies packet into RAM by itself, no CPU needed
//             v
//    ===================
//    RX RING  (HW/DPDK)    ← NIC fills this descriptor ring automatically
//    ===================
//             |
//             | pop_burst() / rte_eth_rx_burst()  ← NO syscall, just reads memory
//             v
//         RX lcore             ← THIS FILE: DpdkPath::lcore_poll_loop() runs here
//             |                   One CPU core, pinned, busy-polling forever
//             | push to software ring
//             v
//    ===================
//    rte_ring  (SW)           ← in multi-lcore setups: passes mbufs to worker
//    ===================       (in our simulation: lcore_poll_loop does both RX + work)
//             |
//             | pop from ring
//             v
//       Worker lcore           ← process_burst() does: PDR → QER → FAR
//             |
//             | push result to TX ring
//             v
//    ===================
//    rte_ring  (SW)
//    ===================
//             |
//             v
//         TX lcore             ← rte_eth_tx_burst() → NIC TX ring → wire
//             |
//             v
//    ===================
//    TX RING  (HW/DPDK)
//    ===================
//             |
//             v
//           WIRE
//
// OUR SIMULATION: lcore_poll_loop() merges RX lcore + Worker lcore into one.
// The SPSCRing rx_ring_ stands in for the HW RX RING + SW rte_ring combined.
// ─────────────────────────────────────────────────────────────────────────────
//
// KEY DPDK CONCEPTS IN THIS FILE:
//
//   1. NO KERNEL INVOLVEMENT
//      In normal Linux socket code: packet → kernel interrupt → sk_buff alloc →
//      recvfrom() syscall → your code. That chain costs 6,000-60,000 ns per packet.
//
//      In DPDK: NIC DMA → mbuf (pre-allocated) → rte_eth_rx_burst() → your code.
//      No interrupt. No syscall. No kernel. Cost: 100-500 ns per packet.
//
//   2. POLL MODE DRIVER (PMD)
//      The lcore does not wait for a packet to arrive. It asks the NIC ring
//      "do you have anything?" in a tight loop. If yes → process. If no → ask again.
//      This burns 100% of one CPU core but eliminates all scheduler/interrupt latency.
//
//      Our simulation:   rx_ring_.pop_burst(burst, BURST_SIZE)
//      Real DPDK call:   rte_eth_rx_burst(port_id, queue_id, burst, BURST_SIZE)
//
//   3. BURST PROCESSING
//      Instead of processing one packet at a time, we grab up to 32 packets at once.
//      Benefits:
//        - One call to now_ns() covers all 32 packets (saves 31 × ~30ns)
//        - PDR hash lookups for same UE are cache-warm by packet 2+ (~50% L1 hit rate)
//        - Branch predictor reaches steady-state by packet 3, reducing branch mispredictions
//      Our BURST_SIZE = 32. Real DPDK default = 32. Adjustable 1-64.
//
//   4. PRE-ALLOCATED MBUFS (no malloc on hot path)
//      Every mbuf was allocated at startup from hugepages. Reusing one costs ~5 cycles.
//      kmalloc would cost 100-500ns. For 10 Mpps that difference is 1-5 seconds of
//      wasted CPU time per second.
//
//   5. ZERO-COPY (in real DPDK)
//      NIC DMA writes packet bytes directly into the mbuf's data region.
//      No kernel copy. No userspace memcpy. Your code reads from the same
//      address the NIC DMA'd into.
//      In our simulation we do one memcpy in rx_enqueue() — flagged in the comment.
//
// TRADE-OFF: 100% CPU usage on the lcore core even at 0 pps (idle).
//   This is intentional. 5G voice (5QI=1) requires <1ms UL latency.
//   You cannot afford even a 100μs OS scheduler wake-up jitter.
//   Production solution: isolcpus kernel boot param removes the core from OS scheduling.
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
    // ── Configuration constants ────────────────────────────────────────────
    //
    // BURST_SIZE = 32
    //   How many packets we pull from the ring in a single pop_burst() call.
    //   This is the same as the BURST argument to rte_eth_rx_burst().
    //   Larger burst → better throughput (amortized overhead) but worse tail latency
    //   (the 32nd packet in a burst waits for the other 31 to fill first).
    //   For voice (5QI=1, latency-sensitive): use 4-8.
    //   For data (5QI=9, throughput-optimized): use 32-64.
    //
    static constexpr size_t BURST_SIZE = 32;

    // RING_SIZE = 4096
    //   Size of our SPSCRing (must be power of 2 for fast bitmask modulo).
    //   Represents how many mbufs can be queued between NIC arrival and lcore pickup.
    //   Real NIC RX descriptor rings: 512-4096 descriptors.
    //   Each descriptor is just a pointer + done_flag. The actual bytes are in the mbuf.
    //
    //   If the ring fills up (producer faster than consumer): rx_enqueue() returns false
    //   and we drop the packet and increment pkts_dropped counter.
    //   In real networks: a full ring means the lcore is overloaded → scale out lcores.
    //
    static constexpr size_t RING_SIZE  = 4096;

    // Constructor: takes a pre-built RuleTable (PDR/FAR/QER rules from SMF via PFCP).
    // The 65536 in MbufPool(65536) means we pre-allocate 65536 mbufs at startup.
    // At 1500 bytes per mbuf × 65536 = ~98MB of packet buffer memory.
    explicit DpdkPath(const RuleTable& rules)
        : rules_(rules), mbuf_pool_(65536), running_(false) {}

    ~DpdkPath() {
        if (running_.load()) stop();
    }

    // ── start() ───────────────────────────────────────────────────────────
    // Launches the lcore poll loop in a background thread.
    //
    // In real DPDK this would be:
    //   rte_eal_init(argc, argv);            // set up hugepages, discover cores
    //   rte_eth_dev_configure(...);           // configure NIC port
    //   rte_eth_rx_queue_setup(...);          // set up RX descriptor ring + mbuf pool
    //   rte_eth_dev_start(port_id);           // start NIC, DMA engine is now running
    //   rte_eal_remote_launch(lcore_fn, arg, lcore_id); // pin to CPU core
    //
    // We skip all that and just launch a std::thread. The logic inside is identical.
    void start() {
        running_.store(true, std::memory_order_seq_cst);
        // std::memory_order_seq_cst = "everyone sees this change immediately"
        // We need the lcore thread to see running_=true before it starts looping.
        worker_ = std::thread([this]{ lcore_poll_loop(); });
    }

    // ── stop() ────────────────────────────────────────────────────────────
    // Signals the lcore to stop and waits for it to drain remaining packets.
    // In real DPDK: rte_eth_dev_stop() → rte_eth_dev_close() → rte_eal_cleanup()
    void stop() {
        running_.store(false, std::memory_order_seq_cst);
        if (worker_.joinable()) worker_.join();
        // join() blocks until the lcore thread exits — it drains the ring first.
    }

    // ── rx_enqueue() ──────────────────────────────────────────────────────
    // SIMULATES: NIC DMA filling a pre-allocated mbuf with the arriving packet.
    //
    // In REAL DPDK you would NOT call this at all. The NIC DMA engine does it
    // automatically. By the time rte_eth_rx_burst() returns a burst, the mbuf
    // is already filled with packet bytes. No CPU involvement in the copy.
    //
    // In OUR SIMULATION: we use this to push a packet into the ring manually,
    // so the lcore_poll_loop() can pick it up. This lets us test the logic
    // without an actual NIC.
    //
    // Returns false if:
    //   - mbuf pool is exhausted (all 65536 mbufs are in-use)
    //   - rx_ring_ is full (ring has RING_SIZE-1 mbufs waiting, lcore can't keep up)
    //   In both cases: packet is dropped, pkts_dropped counter incremented.
    bool rx_enqueue(const Packet& pkt) {
        // Count every packet that enters this function — even ones we drop.
        stats_.pkts_rx.fetch_add(1, std::memory_order_relaxed);
        // memory_order_relaxed = cheapest atomic, no memory barrier, just atomic add.
        // Fine for stats counters where we don't care about ordering with other writes.

        // Step 1: Get a free mbuf from the pool.
        // In real DPDK: rte_pktmbuf_alloc(pool) — returns pointer to a free mbuf
        // from the per-lcore ring cache. Cost: ~5 CPU cycles on a warm cache.
        Mbuf* m = mbuf_pool_.alloc();
        if (!m) {
            // Pool exhausted: all 65536 mbufs are in-flight. Drop the packet.
            // In production: this means you need more mbufs in the pool.
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // Step 2: Fill the mbuf with packet data.
        // In real DPDK: the NIC DMA did this already. Here we memcpy manually.
        // This is the ONE memcpy in our simulation that real DPDK would eliminate.
        m->pkt = pkt;
        m->in_use = true;

        // Step 3: Push mbuf pointer to the RX ring.
        // The lcore_poll_loop() will pop it via pop_burst() on the next iteration.
        // push() is lock-free and costs ~5-10ns (two atomic operations).
        // In real DPDK: the NIC DMA sets the descriptor done_flag instead.
        if (!rx_ring_.push(m)) {
            // Ring is full: lcore can't keep up with incoming rate.
            mbuf_pool_.free_mbuf(m);
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    PathStats& stats() { return stats_; }
    const PathStats& stats() const { return stats_; }

private:
    const RuleTable&           rules_;      // PDR/FAR/QER rules (never changes on fast path)
    SPSCRing<Mbuf*, RING_SIZE> rx_ring_;   // our simulated NIC RX descriptor ring
    MbufPool                   mbuf_pool_; // our simulated hugepage mbuf pool
    std::atomic<bool>          running_;   // signal to stop the lcore thread
    std::thread                worker_;    // the "lcore" thread
    PathStats                  stats_;     // packet counters and latency samples

    // ── lcore_poll_loop() ─────────────────────────────────────────────────
    // THIS IS THE DPDK LCORE FUNCTION.
    // In real DPDK this would be launched as:
    //   rte_eal_remote_launch(lcore_poll_loop, NULL, lcore_id);
    //
    // It runs forever on one CPU core. NEVER:
    //   - calls sleep() or usleep()
    //   - calls mutex_lock() or pthread_cond_wait()
    //   - calls any blocking syscall
    //
    // If the ring is empty: immediately loop back and check again (busy-wait).
    // This is what causes 100% CPU usage. This is intentional.
    //
    // STATE MACHINE of this loop:
    //
    //   ┌─────────────────────────────────────────────┐
    //   │                                             │
    //   ↓                                             │
    //   pop_burst(burst, 32)  ←── n=0: ring empty    │
    //       |                     go back and poll    │
    //       | n > 0: got packets                      │
    //       ↓                                         │
    //   process_burst(burst, n)                       │
    //       |                                         │
    //       | done processing all n packets           │
    //       └─────────────────────────────────────────┘
    //
    void lcore_poll_loop() {
        // burst[] holds pointers to mbufs dequeued from rx_ring_.
        // BURST_SIZE = 32. This is a stack array — ~256 bytes, always in L1 cache.
        // Real DPDK: rte_mbuf* burst[32]; — same pattern.
        Mbuf* burst[BURST_SIZE];

        while (running_.load(std::memory_order_relaxed)) {
            // THE DPDK POLL:
            // Ask the ring: "do you have any packets for me?"
            // If ring empty: n=0, immediately loop back. No sleep. No block.
            // If ring has packets: n=1..32, immediately go to process_burst.
            //
            // Real DPDK equivalent:
            //   uint16_t n = rte_eth_rx_burst(port_id, queue_id, burst, BURST_SIZE);
            //   rte_eth_rx_burst reads the NIC's RX descriptor ring directly.
            //   Each descriptor that the NIC marked "done" gives us one mbuf.
            //
            size_t n = rx_ring_.pop_burst(burst, BURST_SIZE);

            if (n == 0) {
                // Ring is empty right now. This is the busy-wait (spinning).
                // No sleep(). No condition_variable::wait(). No futex.
                // The lcore immediately comes back here and polls again.
                // CPU usage: 100% on this core. That is expected and acceptable.
                // Context: a 5G voice call requires <1ms latency end-to-end.
                //   A sleep(1ms) would violate that SLA alone.
                continue;
            }

            // Got n packets. Process them all as a burst.
            process_burst(burst, n);

            // After process_burst returns: mbufs have been freed back to pool.
            // The lcore immediately polls again.
        }

        // running_ is now false. Drain whatever is left in the ring.
        // Without this drain: packets that were enqueued while we set running_=false
        // would be leaked (mbufs never freed).
        while (true) {
            size_t n = rx_ring_.pop_burst(burst, BURST_SIZE);
            if (n == 0) break;
            process_burst(burst, n);
        }
    }

    // ── process_burst() ───────────────────────────────────────────────────
    // Process a burst of up to BURST_SIZE mbufs.
    // This is the application-level fast path: classify → enforce QoS → forward.
    //
    // WHY BURST PROCESSING IS FASTER THAN ONE AT A TIME:
    //
    //   PER-PACKET (naive):
    //     now_ns()  → lookup  → now_ns()  → lookup  → now_ns()  → ...
    //     30ns each   150ns     30ns each   150ns     30ns each
    //     Total: 180ns × N packets
    //
    //   BURST (DPDK style):
    //     now_ns() ONCE  → lookup[0] → lookup[1] → lookup[2] → ...
    //     30ns ONE TIME    150ns       100ns         100ns
    //     (2nd lookup is cache-warm if same UE → hits L1 instead of L3)
    //     Total: 30ns + ~120ns × N packets  (saves 30ns × (N-1))
    //
    // THREE-STAGE PIPELINE PER PACKET:
    //
    //   Stage 1: PDR (Packet Detection Rule) — "which UE is this packet from?"
    //     - UL: look up TEID in teid_to_pdr_ hash map
    //     - DL: look up UE IP in ueip_to_pdr_ hash map
    //     - If no match: drop + pkts_no_pdr++
    //
    //   Stage 2: QER (QoS Enforcement Rule) — "how fast is this UE allowed?"
    //     - Get QER for this PDR's qer_id
    //     - In production: token bucket check (MBR enforcement)
    //     - If over rate: drop packet
    //     - In our simulation: lookup only, no actual enforcement
    //
    //   Stage 3: FAR (Forwarding Action Rule) — "where does this packet go?"
    //     - FORWARD: send to dst_ip (N6 for UL, gNB for DL)
    //     - DROP: discard (policy-based)
    //     - BUFFER: hold during handover (not simulated here)
    //
    void process_burst(Mbuf** burst, size_t n) {
        // ONE timestamp for the ENTIRE burst.
        // This is the signature DPDK optimization.
        // Alternatives:
        //   rte_rdtsc():     ~1ns  (single RDTSC instruction, real DPDK)
        //   rte_get_timer_cycles(): ~1ns (same, via rte_cycles.h)
        //   std::chrono (ours): ~20-40ns (vDSO call, not as fast but portable)
        // At 32 packets/burst: paying 30ns once vs 32 times = saves 930ns/burst.
        // At 10Mpps / 32 per burst = 312K bursts/sec × 930ns = 290ms/sec saved.
        uint64_t now = Packet::now_ns();

        for (size_t i = 0; i < n; ++i) {
            Mbuf* m = burst[i];
            Packet& pkt = m->pkt;

            // ── STAGE 1: PDR lookup ──────────────────────────────────────
            // "Which UE sent this packet? What rules apply to it?"
            //
            // UL packet: pkt.teid = TEID extracted from GTP-U outer header
            //   teid_to_pdr_.find(teid) → PDR ID → get PDR struct
            //
            // DL packet: pkt.ue_ip = destination IP of inner IP packet
            //   ueip_to_pdr_.find(ue_ip) → PDR ID → get PDR struct
            //
            // Real DPDK: rte_hash_lookup_data(teid_table, &pkt.teid)
            //   Uses cuckoo hash + SIMD (SSE4.2 on x86, NEON on ARM)
            //   Cost: ~80ns (hash compute + 1-2 cache line reads)
            //
            // Our code: unordered_map::find()
            //   Cost: ~150ns (std::hash + linked-list bucket traversal)
            const PDR* pdr = rules_.match_pdr(pkt);
            if (!pdr) {
                // No PDR matched: this packet doesn't belong to any known UE.
                // This can happen when: UE session was torn down (PDR deleted),
                // or packet arrived on wrong port, or TEID mismatch.
                // Action: drop it and count it.
                stats_.pkts_no_pdr.fetch_add(1, std::memory_order_relaxed);
                stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                mbuf_pool_.free_mbuf(m);  // Return mbuf to pool for reuse
                continue;                  // Skip to next packet in burst
            }

            // Fill in the matched rule IDs on the packet struct.
            // Downstream stages (QER, FAR) use these to find their rules
            // without re-doing the hash lookup.
            pkt.pdr_id = pdr->pdr_id;
            pkt.far_id = pdr->far_id;
            pkt.classified = true;

            // ── STAGE 2: QER lookup ──────────────────────────────────────
            // "Is this UE exceeding its allowed bit rate?"
            //
            // QER = QoS Enforcement Rule. The SMF programmed it via PFCP.
            // For voice (QFI=1): MBR = 64kbps. If sending more → drop excess.
            // For video (QFI=2): MBR = 4Mbps.
            // For data  (QFI=9): MBR = 100Mbps (best effort, large cap).
            //
            // In production DPDK: rte_meter_trtcm_color_blind_check()
            //   Token bucket: tokens accumulate at MBR rate.
            //   Packet consumes tokens equal to payload_len bytes.
            //   If insufficient tokens: packet is dropped (RED color).
            //   Cost: ~20ns per packet.
            //
            // In our simulation: we look up the QER struct but don't enforce it.
            const QER* qer = rules_.get_qer(pdr->qer_id);
            (void)qer;  // qer is looked up but rate limiting is not simulated

            // ── STAGE 3: FAR lookup and action ───────────────────────────
            // "What do I actually do with this packet?"
            //
            // FAR.action options:
            //   FORWARD: send to FAR.dst_ip
            //     UL FAR: dst_ip=8.8.8.8 (N6 internet gateway), encap_gtpu=false
            //       → strip outer GTP-U/UDP/IP, send inner IP to internet
            //     DL FAR: dst_ip=10.10.0.1 (gNB N3 IP), dst_port=2152, encap_gtpu=true
            //       → prepend new GTP-U/UDP/IP outer, send to gNB
            //   DROP: discard packet (policy: UE not authorized, handover in progress)
            //   BUFFER: hold for retransmission (during X2/Xn handover)
            const FAR* far = rules_.get_far(pdr->far_id);
            if (!far || far->action == FarAction::DROP) {
                // FAR says DROP, or FAR not found (misconfigured rules).
                pkt.dropped = true;
                stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                mbuf_pool_.free_mbuf(m);
                continue;
            }

            // ── FORWARD ──────────────────────────────────────────────────
            // In real DPDK UL path (N3 → N6):
            //   rte_pktmbuf_adj(m, outer_header_size)  // advance past GTP-U
            //   tx_burst[tx_n++] = m;                   // queue for TX
            //   (at end of burst: rte_eth_tx_burst(N6_PORT, 0, tx_burst, tx_n))
            //
            // In real DPDK DL path (N6 → N3):
            //   char* hdr = rte_pktmbuf_prepend(m, sizeof(gtp_hdr)); // use headroom
            //   fill_gtp_header(hdr, pdr->teid, inner_len);
            //   tx_burst[tx_n++] = m;
            //   (at end of burst: rte_eth_tx_burst(N3_PORT, 0, tx_burst, tx_n))
            //
            // In our simulation: we count the forward and free the mbuf.
            stats_.pkts_forwarded.fetch_add(1, std::memory_order_relaxed);

            // Record latency for this packet using the BURST-level timestamp.
            // In real DPDK: pkt->timestamp (set by NIC hardware at RX time) vs rdtsc.
            // Our now was captured once before the loop → latency of last packet
            // in burst is slightly underestimated (includes waiting for earlier
            // packets to process). This is the intentional burst-level accuracy trade-off.
            stats_.record_latency(pkt.arrival_ns, now);

            // Return mbuf to pool. In real DPDK: rte_pktmbuf_free(m).
            // This pushes the mbuf pointer back to the per-lcore cache.
            // Cost: ~5 CPU cycles. No syscall. No kernel.
            mbuf_pool_.free_mbuf(m);
        }
        // End of burst. Lcore immediately calls pop_burst() again.
    }
};

} // namespace upf
