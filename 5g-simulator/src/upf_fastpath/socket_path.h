#pragma once
// =============================================================================
// socket_path.h — Linux kernel socket-style UPF path (simulation)
//
// This models the overhead sources of a kernel-socket-based UPF. In such an
// architecture (e.g., early free5GC UPF, educational Linux UPF implementations):
//
//   1. Packet arrives at NIC → Linux kernel interrupt handling
//   2. Kernel copies packet through network stack (sk_buff allocation, GTP-U
//      socket processing, inner packet reassembly)
//   3. Application receives packet via recvfrom() syscall (~500-2000ns just for
//      the syscall itself: user↔kernel mode switch + TLB/cache flush)
//   4. Application enqueues to worker thread via shared queue + mutex
//   5. Worker sleeps on condition_variable; receives futex wakeup (~5-50μs)
//   6. Worker processes packet (PDR/FAR/QER lookup), sends via sendto()
//
// OVERHEAD SOURCES (annotated in code):
//
//   a) pthread_mutex_lock() (uncontended): ~20-100ns on modern x86
//      (contended with many producers): ~1-100μs, includes futex syscall
//
//   b) condition_variable::notify_one(): ~200ns-1μs for the signal itself
//      The real cost is on the consumer: it must be scheduled back (~5-50μs)
//
//   c) memcpy to simulate DMA → kernel → app copy chain:
//      A 1400-byte packet = 22 cache lines; copying ~10ns but also pollutes
//      the L1/L2 cache with packet data that overwrites your PDR table entries
//      (cache thrashing). DPDK avoids this by having NIC DMA directly into
//      hugepage-backed mbufs that the application already owns.
//
//   d) Per-packet syscall model: each enqueue represents one recvfrom() call.
//      At 1Mpps, that's 1M syscalls/sec — each costing ~500-2000ns = 50-200%
//      CPU overhead before any packet processing happens.
//
// WHY THIS LIMITS THROUGHPUT:
//   Typical kernel-based UPF: 200Kpps - 1Mpps per core
//   DPDK-based UPF: 10-40Mpps per core (10-100x more)
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
    // Simulate the DMA→kernel→userspace copy chain.
    // In real kernel networking: sk_buff is allocated in kernel, data is copied
    // to userspace via recvfrom(). For DPDK with zero-copy, this memcpy doesn't
    // happen — the NIC DMA writes directly into userspace hugepages.
    static constexpr bool SIMULATE_COPY = true;

    // Maximum queue depth before back-pressure (drop on overflow).
    // In production: Linux socket receive buffer (SO_RCVBUF, default 212992 bytes
    // = ~150 packets). We use a deeper queue for simulation clarity.
    static constexpr size_t MAX_QUEUE_DEPTH = 4096;

    explicit SocketPath(const RuleTable& rules) : rules_(rules), running_(false) {}

    ~SocketPath() {
        if (running_.load()) stop();
    }

    // ── start ────────────────────────────────────────────────────────────
    // Launches the worker thread. In a real kernel UPF, this corresponds to
    // calling recvfrom() in a loop — blocking on each packet.
    void start() {
        running_.store(true, std::memory_order_seq_cst);
        worker_ = std::thread([this]{ worker_loop(); });
    }

    // ── stop ─────────────────────────────────────────────────────────────
    void stop() {
        running_.store(false, std::memory_order_seq_cst);
        cv_.notify_one();   // wake blocked worker so it can check running_
        if (worker_.joinable()) worker_.join();
    }

    // ── enqueue ─────────────────────────────────────────────────────────
    // Simulates: recvfrom() completing + handing packet to worker.
    // OVERHEAD A: memcpy (~10-30ns, plus cache pollution)
    // OVERHEAD B: mutex lock (uncontended ~20-100ns, contended ~1-100μs)
    // OVERHEAD C: cv.notify_one() = futex(FUTEX_WAKE) syscall (~200ns-1μs)
    bool enqueue(const Packet& pkt) {
        stats_.pkts_rx.fetch_add(1, std::memory_order_relaxed);

        Packet copy;
        if (SIMULATE_COPY) {
            // OVERHEAD A: memcpy simulates kernel→userspace copy.
            // At 1Mpps with 1400-byte packets: 1.4GB/s of memcpy = significant
            // L1/L2 cache pressure that evicts your PDR hash table.
            std::memcpy(&copy, &pkt, sizeof(Packet));
        } else {
            copy = pkt;
        }

        {
            // OVERHEAD B: mutex lock
            // Even uncontended, pthread_mutex_lock() performs a CAS on the
            // mutex word. If another thread holds it, we enter the kernel
            // (futex syscall) and sleep until the owner calls unlock.
            std::lock_guard<std::mutex> lk(mtx_);
            if (queue_.size() >= MAX_QUEUE_DEPTH) {
                stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                return false;  // back-pressure: drop
            }
            queue_.push(copy);
        }

        // OVERHEAD C: condition_variable::notify_one()
        // This calls futex(FUTEX_WAKE) — a syscall that wakes one waiter.
        // The waiter (worker thread) still needs to be scheduled by the OS
        // scheduler. Typical scheduler latency: 5-50μs (Linux CFS default
        // 4ms timeslice but wakeup latency is much shorter — still ~μs range).
        cv_.notify_one();
        return true;
    }

    PathStats& stats() { return stats_; }
    const PathStats& stats() const { return stats_; }

private:
    const RuleTable& rules_;
    std::queue<Packet> queue_;

    // OVERHEAD: These synchronization primitives are the main bottleneck.
    // Every packet requires at minimum one mutex lock + one cv notify.
    std::mutex              mtx_;
    std::condition_variable cv_;
    std::atomic<bool>       running_;
    std::thread             worker_;
    PathStats               stats_;

    // ── worker_loop ──────────────────────────────────────────────────────
    // The consumer thread. In a real kernel UPF this would be the recvfrom()
    // thread, blocking on each packet individually.
    //
    // SPURIOUS WAKEUP HANDLING: condition_variable::wait() can return even
    // when the condition is not satisfied (spurious wakeup, allowed by POSIX).
    // We must recheck queue_.empty() after every wake — the predicate lambda
    // handles this correctly.
    void worker_loop() {
        while (true) {
            Packet pkt;
            {
                std::unique_lock<std::mutex> lk(mtx_);
                // Wait with predicate: handles spurious wakeups correctly.
                // The lambda is rechecked after every wakeup. If the queue
                // is empty and running_ is false, we exit.
                cv_.wait(lk, [this]{
                    return !queue_.empty() || !running_.load(std::memory_order_relaxed);
                });

                if (queue_.empty()) {
                    // running_ is false and queue is empty: clean shutdown
                    break;
                }
                pkt = queue_.front();
                queue_.pop();
            }
            process_one(pkt);
        }

        // Drain remaining packets after stop() is called
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

    // ── process_one ─────────────────────────────────────────────────────
    // Per-packet processing pipeline: PDR → QER → FAR.
    // In a real socket-based UPF, each step may involve additional
    // data copies and syscalls (sendto for forwarding).
    void process_one(Packet& pkt) {
        // PDR lookup: O(1) hash map lookup
        const PDR* pdr = rules_.match_pdr(pkt);
        if (!pdr) {
            stats_.pkts_no_pdr.fetch_add(1, std::memory_order_relaxed);
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        pkt.pdr_id = pdr->pdr_id;
        pkt.far_id = pdr->far_id;
        pkt.classified = true;

        // QER lookup (not enforcing rate in simulation, just lookup)
        const QER* qer = rules_.get_qer(pdr->qer_id);
        (void)qer;  // In production: token bucket check here

        // FAR lookup and action
        const FAR* far = rules_.get_far(pdr->far_id);
        if (!far || far->action == FarAction::DROP) {
            pkt.dropped = true;
            stats_.pkts_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // FORWARD: In a real kernel UPF, this is sendto() — another syscall.
        // At 1Mpps: 1M sendto() calls/sec = major overhead.
        stats_.pkts_forwarded.fetch_add(1, std::memory_order_relaxed);
        stats_.record_latency(pkt.arrival_ns, Packet::now_ns());
    }
};

} // namespace upf
