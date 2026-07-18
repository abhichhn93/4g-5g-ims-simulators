#pragma once
// =============================================================================
// mbuf_pool.h — Simulated memory buffer pool (rte_mempool / rte_mbuf analogue)
//
// WHY PRE-ALLOCATION AND HUGEPAGES MATTER IN DPDK
// ─────────────────────────────────────────────────
// In a kernel-based UPF, each packet allocation calls kmalloc() or involves
// the Linux page allocator. This is:
//   - ~100-500ns per allocation (vs ~1-5ns for pre-allocated pool)
//   - Non-deterministic: the allocator may swap pages, handle fragmentation
//   - TLB-miss prone: 4KB pages mean a 1Mpps stream needs ~250K TLB entries/sec
//
// DPDK's solution: rte_mempool
//   1. At startup, allocate a large contiguous block using 2MB hugepages.
//      2MB pages reduce TLB misses by 512x vs 4KB pages (one TLB entry covers
//      512 packets at 1500 bytes each instead of one 4KB page).
//   2. Pre-populate with rte_mbuf objects. Each rte_mbuf is cache-aligned
//      and includes:
//        - rte_mbuf metadata (64 bytes): pkt_len, data_off, port, ol_flags, ...
//        - headroom: space before the packet (128 bytes default, for prepending
//          GTP-U / outer IP / MAC without memmove)
//        - data: the actual packet bytes
//   3. alloc = pop from per-lcore cache (lock-free, ~5 cycles)
//      free  = push back to cache (lock-free, ~5 cycles)
//
// OUR SIMULATION:
//   - std::vector<Mbuf> pool_: equivalent to the hugepage-backed mempool region
//   - atomic bump-pointer alloc(): simple and fast, no free list
//     (Real rte_mempool uses a ring per lcore + global fallback ring)
//   - free_mbuf(): just marks in_use=false (no actual deallocation)
//   - In the benchmark, we pre-size to 65536 mbufs — enough for in-flight
//     packets at 32-burst DPDK processing rates
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <vector>
#include <atomic>
#include "packet.h"

namespace upf {

// Mbuf: the basic unit of packet memory in DPDK.
// In real DPDK, rte_mbuf is a carefully padded 128-byte structure to fit
// exactly two cache lines, with the hot fields (pkt_len, data_len, port,
// ol_flags) in the first cache line and the cold metadata in the second.
struct Mbuf {
    Packet pkt;
    bool   in_use{false};

    // In real rte_mbuf: buf_addr (void* to the data buffer),
    // data_off (offset into buffer), next (for chained mbufs),
    // nb_segs (number of segments for jumbo frames), refcnt, etc.
};

class MbufPool {
public:
    explicit MbufPool(size_t n) : pool_(n), next_(0) {}

    // ── alloc ────────────────────────────────────────────────────────────
    // Atomic bump-pointer: fast, no lock, no free list.
    // In real DPDK: rte_pktmbuf_alloc() pops from a per-lcore ring cache
    // (~5 cycles on a warm cache). Our bump-pointer is similar in overhead.
    //
    // Returns nullptr when pool is exhausted (caller should track and reuse).
    Mbuf* alloc() {
        uint32_t idx = next_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= pool_.size()) {
            // Pool exhausted: wrap around (for long-running benchmarks)
            // In real DPDK this would return nullptr; caller drops the packet.
            next_.store(0, std::memory_order_relaxed);
            idx = 0;
        }
        pool_[idx].in_use = true;
        return &pool_[idx];
    }

    // ── free_mbuf ────────────────────────────────────────────────────────
    // In real DPDK: rte_pktmbuf_free() pushes back to per-lcore cache.
    // The lcore can immediately reuse it without going to the global ring.
    // This is why the "producer lcore allocates, consumer lcore frees" pattern
    // needs careful design in DPDK — cross-lcore free is slower.
    void free_mbuf(Mbuf* m) {
        if (m) m->in_use = false;
    }

    size_t pool_size() const { return pool_.size(); }

private:
    std::vector<Mbuf> pool_;
    std::atomic<uint32_t> next_;
};

} // namespace upf
