#pragma once
// =============================================================================
// socket_path.h — Linux kernel socket-style UPF path (simulation)
//
// ─────────────────────────────────────────────────────────────────────────────
// WHAT THIS FILE MODELS:
//
// This is the SLOW PATH. It shows how a UPF would work using normal Linux
// socket programming — the way you already know from socket(), bind(), recvfrom().
// It models all the overhead sources so you can understand WHY DPDK is 10-100x faster.
//
// ─────────────────────────────────────────────────────────────────────────────
// COMPARE: SOCKET PATH vs DPDK PATH
//
// SOCKET PATH (this file):               DPDK PATH (dpdk_path.h):
//
//         WIRE                                   WIRE
//           |                                      |
//           v                                      v
//       +-------+                             +-------+
//       |  NIC  |                             |  NIC  |
//       +-------+                             +-------+
//           |                                      |
//           | NIC raises INTERRUPT                 | NIC DMA (no interrupt)
//           v                                      v
//       KERNEL                               RX RING (HW)
//       interrupt handler                    NIC fills mbuf directly
//       ~1-5μs overhead                      ~0ns overhead
//           |                                      |
//           v                                      v
//       KERNEL                               lcore busy-poll
//       sk_buff alloc (kmalloc)              pop_burst() ← lock-free
//       ~100-500ns                           ~5-10ns
//           |                                      |
//           v                                      v
//       KERNEL                               mbuf (pre-allocated)
//       recvfrom() SYSCALL                   No syscall needed
//       user↔kernel mode switch              ~0ns
//       ~500-2000ns                               |
//           |                                      v
//           v                               process_burst()
//       USERSPACE                            PDR lookup ~80ns
//       mutex lock → push to queue           FAR lookup ~30ns
//       ~20-100ns                                  |
//           |                                      v
//           v                               rte_eth_tx_burst()
//       cv.notify_one()                     No syscall
//       futex syscall                        ~50-200ns total
//       ~200-1000ns                    ─────────────────────────
//           |                          TOTAL: ~100-500ns/packet
//           v
//       Worker thread WAKES
//       OS scheduler latency
//       ~5000-50000ns
//           |
//           v
//       PDR/FAR/QER lookup
//       ~150ns (unordered_map)
//           |
//           v
//       sendto() SYSCALL
//       ~500-2000ns
//  ─────────────────────────────────
//  TOTAL: ~6000-60000ns/packet
//  (6-60 MICROSECONDS per packet)
//
// ─────────────────────────────────────────────────────────────────────────────
// OVERHEAD SOURCES IN ORDER OF COST:
//
//  A) OS SCHEDULER WAKEUP (BIGGEST COST):
//     condition_variable::wait() puts the worker thread to sleep.
//     cv.notify_one() wakes it. But the OS scheduler decides WHEN the thread
//     actually runs. Linux CFS default: minimum wakeup latency ~5μs, typical ~50μs.
//     At 1Mpps: spending 50μs per packet means 50 seconds of scheduler wait per second.
//     Impossible. This is why socket-based UPFs cap out at ~200K-1Mpps.
//
//  B) SYSCALLS (SECOND BIGGEST):
//     recvfrom(): mode switch from userspace to kernel, TLB flush, cache flush,
//     register save/restore. Cost: ~500-2000ns each.
//     sendto(): same cost again.
//     At 1Mpps: 2M syscalls/sec × 1000ns = 2 seconds of syscall overhead per second.
//
//  C) MUTEX (SMALLER BUT STILL SIGNIFICANT):
//     pthread_mutex_lock(): CAS instruction (~20ns uncontended).
//     If contested (another thread holds it): futex(FUTEX_WAIT) syscall = expensive.
//     At 1Mpps with a single mutex protecting the queue: ~50% contention rate.
//
//  D) MEMCPY (CACHE POLLUTION):
//     Kernel copies sk_buff → userspace buffer. ~10-30ns per 1400-byte packet.
//     The real cost: memcpy pollutes L1/L2 cache with packet bytes,
//     evicting your PDR hash table entries. Next PDR lookup = L3 cache miss = +50ns.
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY DO WE SIMULATE THIS?
//
// To show the contrast. Our demo (upf_fastpath_demo.cpp) runs both paths and prints
// the latency comparison. Running socket path first makes the DPDK improvement visible.
//
// In production, a real socket-based UPF (some early free5GC versions) uses:
//   bind() on GTP-U socket (port 2152), recvfrom() in a loop, process, sendto()
// This works fine at low traffic (lab testing) but cannot handle production load.
// =============================================================================

#include <queue>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <thread>
#include <cstring>
#include "packet.h"
#include "pfcp_rules.h"
#include "stats.h"

namespace upf {

class SocketPath {
public:
    // ── Configuration ─────────────────────────────────────────────────────
    //
    // SIMULATE_COPY = true:
    //   We do a memcpy() in enqueue() to simulate the kernel→userspace copy.
    //   In a real socket UPF: kernel calls copy_to_user() to move sk_buff data
    //   to your userspace buffer. We simulate it with memcpy(&copy, &pkt, sizeof(Packet)).
    //   This is one of the 2-3 copies a kernel packet goes through (NIC→skb, skb→user).
    //
    // MAX_QUEUE_DEPTH = 4096:
    //   Simulates the socket receive buffer (SO_RCVBUF).
    //   Default Linux SO_RCVBUF = 212,992 bytes ≈ 150 packets at 1400 bytes each.
    //   If the queue fills up (producer faster than consumer): we drop packets.
    //   In real systems: excess packets sit in the NIC ring until it fills, then drops.
    static constexpr bool   SIMULATE_COPY   = true;
    static constexpr size_t MAX_QUEUE_DEPTH = 4096;

    explicit SocketPath(const RuleTable& rules) : rules_(rules), running_(false) {}

    ~SocketPath() {
        if (running_.load()) stop();
    }

    // ── start() ───────────────────────────────────────────────────────────
    // Launch the worker thread. In a real kernel socket UPF:
    //   bind(sock, (sockaddr*)&addr, sizeof(addr));  // bind to GTP-U port 2152
    //   while (true) { recvfrom(sock, buf, sizeof(buf), 0, ...); process(buf); }
    // The recvfrom() blocks waiting for packets.
    // We simulate this with a worker thread waiting on a condition_variable.
    void start() {
        running_.store(true, std::memory_order_seq_cst);
        worker_ = std::thread([this]{ worker_loop(); });
    }

    // ── stop() ────────────────────────────────────────────────────────────
    void stop() {
        running_.store(false, std::memory_order_seq_cst);
        // Wake the worker so it can check running_ and exit.
        // In a real socket UPF: you'd close the socket, causing recvfrom() to return EBADF.
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    // ── enqueue() ─────────────────────────────────────────────────────────
    // Simulates: packet arrives via recvfrom() → handed to worker thread.
    //
    // OVERHEAD A — memcpy (~10-30ns + cache pollution):
    //   Simulates the kernel→userspace copy (copy_to_user in kernel).
    //   In real kernel networking, this happens 2-3 times:
    //     1. NIC DMA → sk_buff (kernel memory)
    //     2. sk_buff → recvfrom() buffer (userspace memory via copy_to_user)
    //   In DPDK: zero-copy — NIC DMA writes directly into userspace mbuf. 0 copies.
    //
    // OVERHEAD B — mutex lock (~20-100ns uncontended, ~1-100μs contended):
    //   std::lock_guard acquires the mutex. If another thread holds it:
    //     → futex(FUTEX_WAIT) syscall → thread sleeps → kernel wakeup needed
    //   In DPDK: SPSCRing uses only atomic loads/stores. No locks. ~1-5ns.
    //
    // OVERHEAD C — cv.notify_one() (~200ns-1μs just for the signal):
    //   futex(FUTEX_WAKE) syscall tells OS to schedule the worker thread.
    //   The worker doesn't actually run until the OS scheduler decides to run it.
    //   Linux CFS minimum scheduler latency: ~5-50μs.
    //   In DPDK: no notification needed. The lcore is already polling. 0ns.
    //
    bool enqueue(const Packet& pkt) {
        stats_.pkts_rx.fetch_add(1, std::memory_order_relaxed);

        Packet copy;
        if (SIMULATE_COPY) {
            // OVERHEAD A: memcpy.
            // Sizeof(Packet) = ~64 bytes = 1 cache line.
            // Beyond the time cost: this evicts the PDR hash table from L1/L2 cache.
            // When the worker does the PDR lookup next, it gets a cache miss (+50-100ns).
            // In DPDK: NIC DMA writes to hugepage-backed mbuf, app reads same memory → 0 copies.
            std::memcpy(&copy, &pkt, sizeof(Packet));
        } else {
            copy = pkt;
        }

        {
            // OVERHEAD B: mutex lock.
            // std::lock_guard is RAII — acquires on construction, releases on destruction.
            // The mutex protects queue_ (a std::queue, not thread-safe by itself).
            //
            // WHAT HAPPENS INSIDE pthread_mutex_lock():
            //   1. Atomic CAS on mutex word: try to set it from 0 (free) to 1 (held)
            //   2. If CAS succeeds (uncontended): done. Cost: ~20-50ns.
            //   3. If CAS fails (someone holds it): futex(FUTEX_WAIT) syscall
            //      → thread goes to sleep → woken later → try again
            //      → Cost: 100ns-1ms depending on contention
            std::lock_guard<std::mutex> lk(mtx_);

            if (queue_.size() >= MAX_QUEUE_DEPTH) {
                // Queue full: back-pressure. Drop the packet.
                // In real systems: this manifests as packet loss visible in Wireshark/tcpdump.
                stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
            queue_.push(copy);
        } // mutex released here (lk destructor)

        // OVERHEAD C: cv.notify_one().
        // Calls futex(FUTEX_WAKE, 1) — wakes ONE thread waiting on cv_.wait().
        // The worker thread is now in the OS run queue but NOT running yet.
        // The OS scheduler will run it when it gets CPU time.
        // Typical scheduler latency: 5μs (good Linux kernel) to 50μs (busy system).
        //
        // In DPDK: the lcore is ALREADY running (busy-polling). No notification needed.
        // By the time we would have called cv.notify_one(), the DPDK lcore has
        // already processed the packet. Total notification cost in DPDK: 0ns.
        cv_.notify_one();
        return true;
    }

    PathStats& stats() { return stats_; }
    const PathStats& stats() const { return stats_; }

private:
    const RuleTable& rules_;

    // std::queue is the kernel socket buffer equivalent (SO_RCVBUF).
    // Not thread-safe by itself — protected by mtx_.
    std::queue<Packet> queue_;

    // These two primitives are the main bottleneck of the socket path.
    // EVERY single packet requires:
    //   - At least one mutex_lock + mutex_unlock (atomic CAS + memory barrier)
    //   - At least one cv.notify_one() (futex syscall)
    //   - At least one cv.wait() wakeup (OS scheduler + context switch)
    std::mutex              mtx_;   // protects queue_
    std::condition_variable cv_;    // signals worker when queue_ is non-empty

    std::atomic<bool> running_;     // set to false to signal graceful shutdown
    std::thread       worker_;      // the "application worker" thread
    PathStats         stats_;

    // ── worker_loop() ─────────────────────────────────────────────────────
    // The consumer thread. In a real kernel socket UPF:
    //   this is the thread calling recvfrom() in a tight loop.
    //   recvfrom() blocks (sleeps) when no packet is available.
    //   We simulate this with condition_variable::wait().
    //
    // SPURIOUS WAKEUPS:
    //   The POSIX standard allows condition_variable::wait() to return even when
    //   no one called notify(). This is called a "spurious wakeup."
    //   You MUST check the condition again after every wakeup.
    //   The lambda predicate handles this: cv_.wait(lk, [this]{ return !empty; })
    //   If the condition is false (spurious), wait() immediately sleeps again.
    //   If the condition is true (real wakeup), wait() returns.
    //
    void worker_loop() {
        while (true) {
            Packet pkt;
            {
                std::unique_lock<std::mutex> lk(mtx_);

                // BLOCKING WAIT:
                // If queue is empty AND running_ is true: sleep here.
                // cv_.wait() atomically: releases the mutex AND puts thread to sleep.
                // When woken: reacquires mutex AND rechecks the lambda.
                //
                // The lambda predicate: "wake me when queue is non-empty OR we're stopping"
                // Handles spurious wakeups automatically: if condition is still false,
                // wait() goes back to sleep.
                cv_.wait(lk, [this]{
                    return !queue_.empty() || !running_.load(std::memory_order_relaxed);
                });

                if (queue_.empty()) {
                    // running_ is false and queue is empty: clean shutdown requested
                    break;
                }

                // Dequeue one packet. The mutex ensures no other thread reads queue_.
                pkt = queue_.front();
                queue_.pop();
            } // mutex released here — enqueue() can now add more packets

            // Process the packet (PDR → QER → FAR)
            process_one(pkt);
        }

        // Drain any remaining packets after stop() was called.
        // Without this: packets enqueued between running_=false and thread exit are lost.
        while (true) {
            Packet pkt;
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (queue_.empty()) break;
                pkt = queue_.front();
                queue_.pop();
            }
            process_one(pkt);
        }
    }

    // ── process_one() ─────────────────────────────────────────────────────
    // Per-packet processing: PDR → QER → FAR.
    // This is identical to what happens in DPDK's process_burst() — same lookups.
    // The difference is HOW we got here:
    //   Socket path: mutex lock + cv wait + context switch just to reach this function
    //   DPDK path:   already in the lcore loop, no overhead to reach process_burst()
    //
    void process_one(Packet& pkt) {
        // PDR lookup: "which UE sent this and what rules apply?"
        // unordered_map::find() = ~150ns (hash + possible bucket chain traversal)
        // Real DPDK rte_hash_lookup_data() = ~80ns (cuckoo hash, SIMD accelerated)
        const PDR* pdr = rules_.match_pdr(pkt);
        if (!pdr) {
            // No PDR match: packet from unknown UE, or UE session already torn down.
            stats_.pkts_no_pdr.fetch_add(1, std::memory_order_relaxed);
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        pkt.pdr_id = pdr->pdr_id;
        pkt.far_id = pdr->far_id;
        pkt.classified = true;

        // QER lookup: "is this UE within their allowed rate?"
        // In production: token bucket check (rte_meter_trtcm). Not enforced here.
        const QER* qer = rules_.get_qer(pdr->qer_id);
        (void)qer;  // Lookup done, but no rate enforcement in simulation

        // FAR lookup: "what do I do with this packet?"
        const FAR* far = rules_.get_far(pdr->far_id);
        if (!far || far->action == FarAction::DROP) {
            pkt.dropped = true;
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // FORWARD.
        // In a real socket UPF: sendto(sock, buf, pkt_len, 0, &dst_addr, sizeof(dst_addr))
        // sendto() = another syscall = another 500-2000ns overhead.
        // At 1Mpps: 1M sendto() calls/sec = 0.5-2 seconds of pure syscall overhead per second.
        // In DPDK: rte_eth_tx_burst() = NOT a syscall, writes to TX ring directly. ~50-200ns burst.
        stats_.pkts_forwarded.fetch_add(1, std::memory_order_relaxed);
        stats_.record_latency(pkt.arrival_ns, Packet::now_ns());
    }
};

} // namespace upf
