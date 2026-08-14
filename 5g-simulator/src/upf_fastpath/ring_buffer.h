#pragma once
// =============================================================================
// ring_buffer.h — Lock-free SPSC ring buffer
//
// ─────────────────────────────────────────────────────────────────────────────
// WHERE THIS FITS IN THE DPDK PIPELINE:
//
//           WIRE
//             |
//             v
//         +-------+
//         |  NIC  |
//         +-------+
//             |
//             | DMA
//             v
//    ===================
//    RX RING  (HW/DPDK)    ← THIS CLASS simulates this hardware ring
//    ===================      In real DPDK: NIC descriptor ring (hardware)
//             |                In our code:  SPSCRing<Mbuf*, 4096>
//             | rte_eth_rx_burst() / pop_burst()
//             v
//         RX lcore
//             |
//             | rte_ring_enqueue_burst() / push()  ← also uses rte_ring SW
//             v
//    ===================
//    rte_ring  (SW)        ← THIS CLASS also simulates this software ring
//    ===================      Between lcores (RX → Worker → TX pipeline)
//             |
//             v
//       Worker lcore
//             |
//             v
//    ===================
//    rte_ring  (SW)
//    ===================
//             |
//             v
//         TX lcore → TX RING (HW) → WIRE
//
// ─────────────────────────────────────────────────────────────────────────────
// WHAT THIS CLASS IS:
//
// An SPSCRing is a queue between one producer (the thing writing packets) and
// one consumer (the thing reading packets). SPSC = Single Producer Single Consumer.
//
// Think of it like a conveyor belt in a factory:
//   - The factory loader puts boxes on the LEFT end (push = producer)
//   - The factory worker takes boxes off the RIGHT end (pop = consumer)
//   - The belt holds N boxes maximum
//   - The loader doesn't need permission from the worker to put boxes on
//   - The worker doesn't need permission from the loader to take boxes off
//   - They never touch the same box at the same time → NO LOCK NEEDED
//
// ─────────────────────────────────────────────────────────────────────────────
// WHY NOT JUST USE A MUTEX-PROTECTED QUEUE?
//
// A std::mutex approach:
//   Producer: mutex_lock() → queue.push() → mutex_unlock() → cv.notify_one()
//   Consumer: cv.wait() → mutex_lock() → queue.pop() → mutex_unlock()
//
// Cost per push+pop:
//   mutex_lock()     : CAS + possible futex(FUTEX_WAIT) syscall → ~20ns-1ms
//   cv.notify_one()  : futex(FUTEX_WAKE) syscall → ~200ns
//   context switch   : consumer thread wakes from sleep → ~5,000-50,000ns
//   cache ping-pong  : mutex word bounces between CPU cores → ~100-200ns/op
//   Total: 5,000-50,000 nanoseconds per packet at best
//
// This SPSCRing approach:
//   push(): atomic store to head_ → ~1-5ns
//   pop():  atomic load from head_, write to tail_ → ~1-5ns
//   No locks. No syscalls. No thread wakeups.
//   Total: ~5-10 nanoseconds per push+pop
//
// Speed difference: 1000-10000× faster at moving packets between threads.
//
// ─────────────────────────────────────────────────────────────────────────────
// VISUAL: HOW THE RING WORKS
//
// N = 8 (example, our code uses N=4096)
// head_ = where the PRODUCER will write NEXT
// tail_ = where the CONSUMER will read NEXT
// "empty" = head_ == tail_
// "full"  = (head_ + 1) % N == tail_  (one slot always kept empty to detect full)
//
// INITIAL STATE (empty):
//   ┌────┬────┬────┬────┬────┬────┬────┬────┐
//   │    │    │    │    │    │    │    │    │
//   └────┴────┴────┴────┴────┴────┴────┴────┘
//    [0]  [1]  [2]  [3]  [4]  [5]  [6]  [7]
//    h,t
//   head_=0, tail_=0 → (h==t) means empty → pop returns false
//
// AFTER push(mb0):
//   producer writes buf_[0]=mb0, then moves head_ to 1
//   ┌────┬────┬────┬────┬────┬────┬────┬────┐
//   │mb0 │    │    │    │    │    │    │    │
//   └────┴────┴────┴────┴────┴────┴────┴────┘
//    [0]  [1]  [2]  [3]  [4]  [5]  [6]  [7]
//    t    h
//   head_=1, tail_=0 → not empty, consumer can pop
//
// AFTER push(mb1), push(mb2):
//   ┌────┬────┬────┬────┬────┬────┬────┬────┐
//   │mb0 │mb1 │mb2 │    │    │    │    │    │
//   └────┴────┴────┴────┴────┴────┴────┴────┘
//    t              h
//   head_=3, tail_=0
//
// AFTER pop_burst(out, 3):  consumer reads mb0,mb1,mb2, moves tail_ to 3
//   ┌────┬────┬────┬────┬────┬────┬────┬────┐
//   │mb0 │mb1 │mb2 │    │    │    │    │    │
//   └────┴────┴────┴────┴────┴────┴────┴────┘
//                   h,t
//   head_=3, tail_=3 → (h==t) means empty again
//   out[] = {mb0, mb1, mb2}, returns 3
//
// WRAP-AROUND (head near end):
//   head_=7, tail_=5 (two items in ring: buf_[5] and buf_[6])
//   ┌────┬────┬────┬────┬────┬────┬────┬────┐
//   │    │    │    │    │    │mb5 │mb6 │    │
//   └────┴────┴────┴────┴────┴────┴────┴────┘
//                              t         h
//   After push(mb7): next_h = (7+1) & (8-1) = 8 & 7 = 0 → WRAPS TO 0!
//   ┌────┬────┬────┬────┬────┬────┬────┬────┐
//   │    │    │    │    │    │mb5 │mb6 │mb7 │
//   └────┴────┴────┴────┴────┴────┴────┴────┘
//    h                       t
//   head_=0, tail_=5 → three items: [5],[6],[7], pop reads [5] first
//
// FULL RING (N-1 = 7 items when N=8):
//   head_=4, tail_=5 → next_h=(4+1)&7=5 == tail_ → FULL, push returns false
//   ┌────┬────┬────┬────┬────┬────┬────┬────┐
//   │mb0 │mb1 │mb2 │mb3 │    │mb5 │mb6 │mb7 │
//   └────┴────┴────┴────┴────┴────┴────┴────┘
//    [0]  [1]  [2]  [3]  [4]  [5]  [6]  [7]
//                        h    t
//   One slot [4] left empty on purpose: without it, head_==tail_ would mean
//   either "empty" OR "full" — ambiguous. Keeping one empty makes it unambiguous:
//     h==t  → empty
//     (h+1)%N==t → full
//
// WHY N MUST BE POWER OF 2:
//   Normal modulo:  (head + 1) % N   → CPU division instruction ~5-20ns
//   Bitmask trick:  (head + 1) & (N-1) → single AND instruction ~1ns
//   Only works when N is a power of 2 (binary: N-1 is all ones below the set bit).
//   Example: N=8 → N-1=7 → binary 0111 → masking keeps only bits 0,1,2 → values 0-7
//
// ─────────────────────────────────────────────────────────────────────────────
// FALSE SHARING — why head_ and tail_ are on separate cache lines:
//
// A CPU cache line = 64 bytes. If head_ and tail_ share a cache line:
//
//   CPU0 (producer) writes head_:
//     → marks cache line MODIFIED on CPU0's L1 cache
//     → broadcasts "I own this line" to all other CPUs (MESI protocol)
//
//   CPU1 (consumer) reads tail_ (same cache line!):
//     → CPU1 sees line is MODIFIED on CPU0
//     → CPU1 must fetch the line from CPU0 via cache coherence bus
//     → costs ~100-200ns  (same as a RAM access!)
//
//   CPU1 writes tail_:
//     → marks line MODIFIED on CPU1
//     → CPU0 must fetch from CPU1 to read head_
//     → another ~100-200ns
//
//   This "cache line bounce" happens on EVERY push+pop, making our "lock-free"
//   ring nearly as slow as a mutex. This is called FALSE SHARING.
//
// FIX: alignas(64) puts head_ and tail_ on SEPARATE cache lines.
//   CPU0 exclusively owns the head_ cache line.
//   CPU1 exclusively owns the tail_ cache line.
//   No bouncing. Each CPU runs independently.
//
//   BEFORE alignas(64):    AFTER alignas(64):
//   ┌──────────────────┐   ┌──────────────────┐  ┌──────────────────┐
//   │head_  tail_  ... │   │ head_  padding   │  │ tail_  padding   │
//   │  ← same 64 bytes │   │ ← CPU0 owns this │  │ ← CPU1 owns this │
//   └──────────────────┘   └──────────────────┘  └──────────────────┘
//                           no bouncing between CPUs
// =============================================================================

#include <atomic>
#include <cstddef>
#include <array>

namespace upf {

// Template parameters:
//   T = type stored (in dpdk_path.h: T = Mbuf*)
//   N = ring capacity (must be power of 2: 2, 4, 8, ..., 4096, 8192, ...)
template <typename T, size_t N>
class SPSCRing {
    // Compile-time check: fail at compile time if N is not power of 2.
    // (N & (N-1)) == 0 is a classic power-of-2 test:
    //   8 = 1000 in binary, 8-1 = 7 = 0111, 1000 & 0111 = 0 → power of 2 ✓
    //   6 = 0110 in binary, 6-1 = 5 = 0101, 0110 & 0101 = 0100 ≠ 0 → NOT ✓
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");

public:
    // Initialize head and tail to 0. Ring starts empty (head_ == tail_).
    SPSCRing() : head_(0), tail_(0) {}

    // capacity() = N-1 because one slot is always kept empty (full-detection slot).
    // This is a constant expression, so the compiler can optimize away the subtraction.
    static constexpr size_t capacity() { return N - 1; }

    // ── push() ──────────────────────────────────────────────────────────────
    // PRODUCER ONLY. Called by: rx_enqueue() in dpdk_path.h.
    // In real DPDK: this is NOT used — the NIC DMA fills mbufs directly into
    // the hardware descriptor ring without any push() call.
    // In our simulation: rx_enqueue() calls push() to hand the mbuf to the lcore.
    //
    // MEMORY ORDERS explained simply:
    //   relaxed: "just do the atomic operation, don't worry about ordering"
    //   acquire: "I'm reading something the other thread wrote — wait for their writes to be visible"
    //   release: "I've finished writing — let the other thread see my writes"
    //
    // Why these specific orders:
    //   head_.load(relaxed): producer reads its own head, no other thread writes head_
    //   tail_.load(acquire): producer checks consumer's tail — must see consumer's completed pops
    //   buf_[h] = val:       write the actual mbuf pointer BEFORE updating head_
    //   head_.store(release): tell consumer "I've placed something at buf_[h], head_ is now h+1"
    //                         release ensures buf_[h] write is visible before head_ update
    //
    bool push(const T& val) {
        // Read current head (where we'll write) — only producer writes head_, so relaxed is fine
        size_t h = head_.load(std::memory_order_relaxed);

        // Compute next head position using bitmask (fast modulo for power-of-2 N)
        size_t next_h = (h + 1) & (N - 1);

        // Check if ring is full: if next position equals tail_, no room left
        // acquire: we need to see the latest tail_ value the consumer committed
        if (next_h == tail_.load(std::memory_order_acquire)) {
            return false;  // Ring full. Caller should drop packet and count it.
        }

        // Write the value BEFORE advancing head_ (release semantics below ensure ordering)
        buf_[h] = val;

        // Advance head_. The consumer will now see this slot as available.
        // release: ensures buf_[h] = val is committed to memory before head_ is visible to consumer
        head_.store(next_h, std::memory_order_release);
        return true;
    }

    // ── pop() ──────────────────────────────────────────────────────────────
    // CONSUMER ONLY. Called by: the lcore in lcore_poll_loop().
    // Returns false immediately if ring is empty — NO blocking, NO waiting.
    // Caller: immediately loops back and calls pop() again (busy-wait).
    //
    bool pop(T& out) {
        // Read current tail (where we'll read from) — only consumer writes tail_
        size_t t = tail_.load(std::memory_order_relaxed);

        // Check if ring is empty: tail_ == head_ means nothing to read
        // acquire: see the latest head_ value the producer committed (including their buf_[h] write)
        if (t == head_.load(std::memory_order_acquire)) {
            return false;  // Ring empty. Caller spins and tries again.
        }

        // Read the value from the ring
        out = buf_[t];

        // Advance tail_. Producer will now see this slot as free.
        // release: ensures buf_[t] read is done before tail_ is visible to producer
        tail_.store((t + 1) & (N - 1), std::memory_order_release);
        return true;
    }

    // ── pop_burst() ─────────────────────────────────────────────────────────
    // THE KEY DPDK OPTIMIZATION: dequeue up to 'max' items in ONE call.
    //
    // This is the equivalent of rte_eth_rx_burst() or rte_ring_dequeue_burst().
    //
    // WHY BURST IS FASTER THAN pop() N TIMES:
    //
    //   pop() × 32:
    //     For each call: relaxed load of tail, acquire load of head, write out[i],
    //     release store of tail. Each has a store-barrier.
    //     32 separate atomic operations on tail_.
    //
    //   pop_burst(max=32):
    //     ONE acquire load of head (to see how many items are available)
    //     32 plain array reads (no atomics — tail is only updated ONCE at the end)
    //     ONE release store of tail (to tell producer "I consumed n items")
    //     Much less atomic overhead for the same work.
    //
    // Returns: number of items actually dequeued (0 to max).
    // 0 means ring empty → caller immediately loops back (no sleep).
    //
    size_t pop_burst(T* out, size_t max) {
        // Snapshot current tail (our read position)
        size_t t = tail_.load(std::memory_order_relaxed);

        // See how far the producer has gotten (acquire: see their writes to buf_[])
        size_t h = head_.load(std::memory_order_acquire);

        // How many items are available?
        // (h - t) & (N-1) handles wrap-around:
        //   if h=3, t=0: avail=3 (normal case)
        //   if h=1, t=7, N=8: avail=(1-7)&7 = (-6)&7 = (8-6) = 2 (wrapped)
        size_t avail = (h - t) & (N - 1);

        // Don't dequeue more than the caller asked for
        size_t n = (avail < max) ? avail : max;

        // Read n items from the ring (no atomics needed — just array reads)
        for (size_t i = 0; i < n; ++i) {
            out[i] = buf_[(t + i) & (N - 1)];
        }

        if (n > 0) {
            // Tell the producer: "n slots are free again"
            // release: ensures all our reads above are committed before we update tail_
            tail_.store((t + n) & (N - 1), std::memory_order_release);
        }

        return n;
    }

    // How many items are currently in the ring (approximate, for monitoring)
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
    // ── Cache-line alignment ─────────────────────────────────────────────────
    // alignas(64) = force this variable to start at a 64-byte boundary.
    // This puts head_ and tail_ on SEPARATE cache lines.
    // Without this: false sharing causes ~100-200ns stall per push/pop.
    // With this: CPU0 owns head_ cache line, CPU1 owns tail_ cache line. No bouncing.
    //
    // head_ is ONLY written by the producer (push). Written by CPU0.
    // tail_ is ONLY written by the consumer (pop/pop_burst). Written by CPU1.
    // Both are READ by both sides (to check full/empty condition).
    //
    alignas(64) std::atomic<size_t> head_;   // producer's write cursor
    alignas(64) std::atomic<size_t> tail_;   // consumer's read cursor

    // The actual ring storage: an array of N elements.
    // In real DPDK: rte_ring uses a similar fixed-size array backed by hugepages.
    // Our buf_ is just a stack/heap array — fine for simulation.
    std::array<T, N> buf_;
};

} // namespace upf
