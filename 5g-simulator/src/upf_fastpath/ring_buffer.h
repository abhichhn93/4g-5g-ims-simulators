#pragma once
// =============================================================================
// ring_buffer.h — Lock-free SPSC ring buffer
//
// WHY THIS IS FASTER THAN A MUTEX-PROTECTED QUEUE
// ─────────────────────────────────────────────────
// A mutex-protected std::queue has these costs per enqueue+dequeue:
//   1. pthread_mutex_lock()  : CAS + possible futex syscall (kernel involvement)
//   2. pthread_cond_signal() : another futex syscall to wake consumer
//   3. Context switch         : ~1-5μs if consumer was sleeping
//   4. Cache ping-pong        : the mutex word bounces between producer/consumer
//      CPU caches, causing ~100ns coherence traffic even when uncontended
//
// An SPSC (Single-Producer Single-Consumer) lock-free ring avoids ALL of that:
//   1. No locks, no syscalls. Producer writes head_, consumer reads tail_.
//      On x86, stores/loads to aligned 64-bit atomics are ~1ns.
//   2. Cache-line padding (alignas(64)) puts head_ and tail_ on separate
//      cache lines. Without this, both cores would fight over the same 64-byte
//      cache line on every push/pop — "false sharing" costs 50-200ns per op.
//   3. Consumer busy-polls (spin-waits). No futex, no scheduler. This costs
//      CPU time when the ring is empty, but eliminates the 5-50μs wake-up
//      latency of condition_variable::wait(). This is the explicit DPDK tradeoff:
//      burn one CPU core to get sub-microsecond response.
//
// In real DPDK:
//   - rte_ring: lockless ring (supports MPMC, MPSC, SPMC, SPSC variants)
//   - The NIC PMD fills mbufs directly into an rte_ring via DMA
//   - lcore (logical core) polls rte_ring via rte_eth_rx_burst()
//   - No sleep, no interrupt, no kernel — just a tight CAS loop
//
// CONSTRAINTS:
//   - N MUST be a power of 2 (for fast modulo with bitmask)
//   - Only ONE producer thread and ONE consumer thread (SPSC = Single P/C)
//     For multi-producer, you'd need atomic CAS on head_ (rte_ring MPSC mode)
// =============================================================================

#include <atomic>
#include <cstddef>
#include <array>

namespace upf {

template <typename T, size_t N>
class SPSCRing {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");

public:
    SPSCRing() : head_(0), tail_(0) {}

    // Capacity is N-1: one slot is always left empty to distinguish
    // "full" (head+1 == tail) from "empty" (head == tail).
    static constexpr size_t capacity() { return N - 1; }

    // ── push ──────────────────────────────────────────────────────────────
    // Called by producer only. Returns false if ring is full (back-pressure).
    // In real DPDK, a full ring usually means you drop the mbuf and increment
    // a counter — you never block the NIC poll loop.
    //
    // Memory order:
    //   head_.load(relaxed) : producer reads its own head (no sync needed)
    //   tail_.load(acquire) : producer reads consumer's tail — must be at least
    //                         acquire so we see the consumer's completed pops
    //   head_.store(release): publish the new head to the consumer — release
    //                         ensures the payload write (buf_[h] = val) is
    //                         visible to the consumer before it sees the new head
    bool push(const T& val) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t next_h = (h + 1) & (N - 1);
        if (next_h == tail_.load(std::memory_order_acquire)) {
            return false;  // ring full
        }
        buf_[h] = val;
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    // ── pop ──────────────────────────────────────────────────────────────
    // Called by consumer only. Returns false if ring is empty.
    // In DPDK lcore loop: if (pop() returns false) continue; // spin
    bool pop(T& out) {
        size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_.load(std::memory_order_acquire)) {
            return false;  // ring empty
        }
        out = buf_[t];
        tail_.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    // ── pop_burst ────────────────────────────────────────────────────────
    // THE KEY DPDK OPTIMIZATION: dequeue up to 'max' items in one call.
    // Equivalent to rte_eth_rx_burst() or rte_ring_dequeue_burst().
    //
    // Why burst matters:
    //   1. One call to now_ns() covers the entire burst (not per-packet)
    //   2. PDR lookups for consecutive packets from the same UE are cache-warm
    //      (the hash bucket is still in L1 cache from the previous lookup)
    //   3. Branch predictor can optimize the inner loop
    //   4. For 32-packet bursts, the amortized overhead per packet is ~10x
    //      less than processing one at a time
    //
    // Returns number of items actually dequeued (0 to max).
    size_t pop_burst(T* out, size_t max) {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);
        size_t avail = (h - t) & (N - 1);
        size_t n = (avail < max) ? avail : max;
        for (size_t i = 0; i < n; ++i) {
            out[i] = buf_[(t + i) & (N - 1)];
        }
        if (n > 0) {
            tail_.store((t + n) & (N - 1), std::memory_order_release);
        }
        return n;
    }

    size_t size() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & (N - 1);
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

private:
    // ── Cache-line padding ────────────────────────────────────────────────
    // head_ is written by the producer and read by both producer and consumer.
    // tail_ is written by the consumer and read by both.
    // If they share a cache line, each write by either side invalidates the
    // other side's L1 cache entry — "false sharing" — causing ~100-200ns
    // stall per operation even though the data doesn't conflict logically.
    //
    // alignas(64) forces each atomic onto its own 64-byte cache line.
    // This is a standard DPDK/high-perf-networking pattern.

    alignas(64) std::atomic<size_t> head_;   // written by producer
    alignas(64) std::atomic<size_t> tail_;   // written by consumer
    std::array<T, N> buf_;
};

} // namespace upf
