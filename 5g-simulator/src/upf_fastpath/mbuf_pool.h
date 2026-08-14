#pragma once
// =============================================================================
// mbuf_pool.h — Simulated memory buffer pool (rte_mempool / rte_mbuf analogue)
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
//             | DMA → NIC writes packet bytes into mbuf.data
//             ↓            ↑
//    ===================   │
//    RX RING  (HW/DPDK)    │  Each descriptor in the ring points to ONE mbuf.
//    ===================   │  The NIC DMA engine follows the pointer and writes there.
//             |
//             v
//         lcore  ← calls pop_burst(), gets array of mbuf pointers
//             |
//             | process: read mbuf.data → parse TEID → PDR/FAR/QER lookup
//             |
//             | done with packet: mbuf_pool_.free_mbuf(m)
//             v
//   mbuf returns to pool  ← NIC will reuse it for the NEXT incoming packet
//
// ─────────────────────────────────────────────────────────────────────────────
// THE NORMAL WAY (Linux kernel) vs THE DPDK WAY:
//
// NORMAL LINUX (sk_buff):
//   Packet arrives at NIC
//     → kernel allocates sk_buff (kmalloc) on the hot path: ~100-500ns
//     → NIC copies data into sk_buff
//     → your app does recvfrom() → kernel copies to userspace
//     → you process packet
//     → kernel frees sk_buff (kfree): ~100ns
//
//   Problems:
//     - kmalloc is non-deterministic (can touch memory allocator internals)
//     - Each allocation is a different memory address → TLB misses everywhere
//     - At 1Mpps: 1M kmalloc + 1M kfree = 200-1000ms CPU time on allocation alone
//
// DPDK WAY (rte_mempool + rte_mbuf):
//   At startup: allocate ALL mbufs from hugepages
//     → rte_pktmbuf_pool_create(name, 65536, cache=512, 0, 2176, socket_id)
//     → 65536 mbufs, all in a contiguous hugepage region, pinned in RAM
//
//   Each arriving packet:
//     → NIC DMA reuses the SAME mbuf addresses (cycling through the pool)
//     → alloc: rte_pktmbuf_alloc() pops from per-lcore cache: ~5 CPU cycles (~1ns)
//     → no kmalloc. no kernel. no TLB miss (hugepages cover the entire pool)
//
//   After processing:
//     → rte_pktmbuf_free(): pushes back to per-lcore cache: ~5 cycles (~1ns)
//     → pool is ready for the next packet immediately
//
// ─────────────────────────────────────────────────────────────────────────────
// MEMORY LAYOUT — what an mbuf looks like in RAM:
//
// In real DPDK, rte_mbuf is a 128-byte structure (exactly 2 cache lines):
//
//   ┌───────────────────────────────────────────────────────────┐
//   │  CACHE LINE 0 (64 bytes) — HOT metadata, always accessed  │
//   │  ┌──────────┬──────────┬──────────┬──────────┬──────────┐ │
//   │  │ buf_addr │ data_off │ pkt_len  │ data_len │ port     │ │
//   │  │ (8 bytes)│ (2 bytes)│ (4 bytes)│ (2 bytes)│ (2 bytes)│ │
//   │  └──────────┴──────────┴──────────┴──────────┴──────────┘ │
//   │  │ ol_flags │ nb_segs  │ refcnt   │ pkt_type │ ...      │ │
//   │  └──────────┴──────────┴──────────┴──────────┴──────────┘ │
//   ├───────────────────────────────────────────────────────────┤
//   │  CACHE LINE 1 (64 bytes) — COLD metadata, rarely accessed │
//   │  timestamp, hash, userdata, pool pointer, next (chain)... │
//   └───────────────────────────────────────────────────────────┘
//   ↓ (offset by data_off from buf_addr)
//   ┌───────────────────────────────────────────────────────────┐
//   │  HEADROOM (128 bytes default)                             │
//   │  Empty space BEFORE the packet data.                      │
//   │  Used for DL path: prepend GTP-U + UDP + IP outer headers │
//   │  without copying the entire packet — just move data_off   │
//   │  backward into headroom. Cost: ~1ns (pointer arithmetic)  │
//   ├───────────────────────────────────────────────────────────┤
//   │  PACKET DATA (up to 1984 bytes for standard MTU)          │
//   │  This is where the NIC DMA writes the received bytes.     │
//   │  For UL: outer Eth/IP/UDP/GTP-U + inner IP + payload      │
//   │  For DL: inner IP + payload (outer headers go in headroom) │
//   └───────────────────────────────────────────────────────────┘
//   Total mbuf size: 128 (header) + 128 (headroom) + 1984 (data) = 2240 bytes
//   Rounded up to next cache line = 2176 bytes (RTE_MBUF_DEFAULT_BUF_SIZE)
//
// OUR SIMULATION:
//   struct Mbuf { Packet pkt; bool in_use; }
//   Much simpler — Packet holds the metadata we need for demo purposes.
//
// ─────────────────────────────────────────────────────────────────────────────
// HUGEPAGES — why they matter for the mbuf pool:
//
// Normal pages = 4KB each. Hugepages = 2MB each (or 1GB on some systems).
//
// The CPU TLB (Translation Lookaside Buffer) is a cache that maps
// virtual addresses to physical addresses. It has ~1024 entries.
//
// With 65536 mbufs at ~2176 bytes each = 143MB total pool:
//
//   4KB pages:   143MB / 4KB = 35,840 pages → needs 35,840 TLB entries
//                TLB has ~1024 entries → massive TLB thrashing
//                Every 2nd mbuf access → TLB miss → ~100ns extra
//                At 1Mpps → 50ms/second of extra latency from TLB alone
//
//   2MB hugepages: 143MB / 2MB = 72 pages → needs only 72 TLB entries
//                  Entire pool fits in TLB!
//                  Zero TLB misses on mbuf access.
//                  At 1Mpps → ~0ms extra latency from TLB.
//
// Hugepages are locked in RAM (never swapped to disk by the OS).
// DPDK requires hugepages. Setup on Linux:
//   echo 1024 > /proc/sys/vm/nr_hugepages   # allocate 1024 × 2MB = 2GB
//   mount -t hugetlbfs hugetlbfs /mnt/huge  # mount the filesystem
//   # DPDK then allocates from /mnt/huge automatically via rte_eal_init()
//
// ─────────────────────────────────────────────────────────────────────────────
// PER-LCORE CACHE — how alloc/free avoids touching the global ring:
//
//   GLOBAL RING (rte_ring, lock-free MPMC)
//   ┌─────────────────────────────────────────────────────┐
//   │  [mbuf0][mbuf1][mbuf2]...[mbuf65535]                │
//   │  All free mbufs start here after pool creation.     │
//   └─────────────────────────────────────────────────────┘
//          ↕ (only when per-lcore cache is empty/full)
//   PER-LCORE CACHE (one per CPU core, not shared)
//   ┌──────────────────────────────────────────────────────┐
//   │  lcore0_cache: [m10][m11]...[m521]  (up to 512 each)│
//   │  lcore1_cache: [m522][m523]...[m1033]                │
//   │  lcore2_cache: [m1034]...                            │
//   └──────────────────────────────────────────────────────┘
//
//   rte_pktmbuf_alloc(pool) on lcore0:
//     if lcore0_cache not empty:
//       return lcore0_cache.pop()  ← ~5 cycles, NO shared state touched
//     else:
//       bulk-refill lcore0_cache from global ring (512 mbufs at once)
//       return lcore0_cache.pop()
//
//   rte_pktmbuf_free(m) on lcore0:
//     if lcore0_cache not full:
//       lcore0_cache.push(m)  ← ~5 cycles, NO shared state touched
//     else:
//       bulk-drain 256 mbufs from lcore0_cache to global ring
//       lcore0_cache.push(m)
//
//   Result: The hot path (alloc + free) almost never touches the global ring.
//   Each lcore is self-sufficient. No inter-core coordination on the hot path.
//
// Our simulation uses a simpler atomic bump-pointer — same concept, less
// complexity (no per-lcore caches needed since we have one thread).
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <vector>
#include <atomic>
#include "packet.h"

namespace upf {

// ─────────────────────────────────────────────────────────────────────────────
// Mbuf — our simulated rte_mbuf
//
// In real DPDK: rte_mbuf is a carefully padded 128-byte header structure.
// It lives in hugepage memory, is cache-aligned, and has many fields for
// hardware offloads (checksum, VLAN, timestamp, RSS hash, etc.)
//
// Our Mbuf: simpler — wraps a Packet struct (the metadata we care about)
// and an in_use flag. In the real struct you'd have:
//   buf_addr:  pointer to start of data buffer
//   data_off:  offset from buf_addr to first byte of packet (headroom allows prepend)
//   pkt_len:   total length of all segments
//   data_len:  length of this segment's data
//   port:      which NIC port this packet came in on
//   nb_segs:   number of chained mbufs (for jumbo frames > MTU)
//   refcnt:    reference count (free when 0)
//   next:      pointer to next segment (for chained mbufs)
// ─────────────────────────────────────────────────────────────────────────────
struct Mbuf {
    // Our packet metadata (TEID, UE IP, QFI, timestamps, rule IDs)
    // In real rte_mbuf: this data lives in the buffer, accessed via rte_pktmbuf_mtod()
    Packet pkt;

    // Simple in-use flag for our simulation
    // In real rte_mbuf: reference counting (rte_mbuf_refcnt_read/set)
    bool   in_use{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// MbufPool — our simulated rte_mempool
//
// Created ONCE at startup. Pre-allocates all mbufs.
// After creation: no new allocations happen. alloc() and free_mbuf() just
// move pointers around. O(1) guaranteed latency on the hot path.
// ─────────────────────────────────────────────────────────────────────────────
class MbufPool {
public:
    // Constructor: pre-allocate n mbufs.
    // In real DPDK: rte_pktmbuf_pool_create(name, n, cache_size, priv_size,
    //               data_room_size, socket_id)
    //   name:       unique name (pools are globally named in DPDK)
    //   n:          number of mbufs (must be power of 2 - 1, e.g. 65535)
    //   cache_size: per-lcore cache size (512 is typical)
    //   priv_size:  private data per mbuf (0 = none)
    //   data_room:  bytes of packet data space (RTE_MBUF_DEFAULT_BUF_SIZE = 2176)
    //   socket_id:  NUMA node (use same node as the NIC for best performance)
    //
    // NUMA TIP: rte_eth_dev_socket_id(port_id) gives the NUMA node of the NIC.
    //   Allocating the pool on a different NUMA node adds ~100ns per mbuf access
    //   (cross-NUMA memory access goes over QPI/Infinity Fabric interconnect).
    explicit MbufPool(size_t n) : pool_(n), next_(0) {}

    // ── alloc() ────────────────────────────────────────────────────────────
    // Get a free mbuf from the pool. Returns nullptr if pool is exhausted.
    //
    // OUR IMPLEMENTATION: atomic bump-pointer.
    //   next_.fetch_add(1): atomically increment next_ and return old value.
    //   We then return &pool_[old_value].
    //   When we reach the end: wrap around to 0 (for long-running benchmarks).
    //
    // REAL DPDK: rte_pktmbuf_alloc(pool)
    //   1. Check per-lcore cache — if not empty, pop from it (~5 cycles)
    //   2. If empty: bulk-dequeue 512 mbufs from global ring to lcore cache
    //   3. Pop from lcore cache
    //   Total: ~5 cycles on cache hit, ~30 cycles on cache miss.
    //   Compare: kmalloc = ~100-500ns = 300-1500 cycles. DPDK is 100x faster.
    //
    // Returns nullptr when pool exhausted → caller drops the packet.
    // In real systems: if this returns nullptr, you need a bigger pool.
    Mbuf* alloc() {
        // fetch_add is an atomic read-then-add. No locks.
        // Two threads calling alloc() simultaneously each get a different index.
        uint32_t idx = next_.fetch_add(1, std::memory_order_relaxed);

        if (idx >= pool_.size()) {
            // Pool exhausted. Wrap around for our simulation (long-running benchmark).
            // In real DPDK: return nullptr and let the caller drop the packet.
            next_.store(0, std::memory_order_relaxed);
            idx = 0;
        }

        pool_[idx].in_use = true;
        return &pool_[idx];  // Return pointer to mbuf in the pool (hugepage region in real DPDK)
    }

    // ── free_mbuf() ────────────────────────────────────────────────────────
    // Return mbuf to the pool for reuse.
    //
    // OUR IMPLEMENTATION: just mark in_use = false. Our bump-pointer alloc
    // doesn't use a free list — we just cycle through the pool in order.
    //
    // REAL DPDK: rte_pktmbuf_free(m)
    //   1. Decrements refcnt. If refcnt goes to 0:
    //   2. Push to per-lcore cache (~5 cycles if cache not full)
    //   3. If per-lcore cache full: bulk-enqueue 256 mbufs to global ring
    //      then push to lcore cache.
    //
    // The mbuf is NOT zeroed out — the next packet's DMA will overwrite the
    // data bytes anyway. Metadata fields are overwritten by the PMD before
    // returning from rte_eth_rx_burst(). So free is truly just a pointer push.
    void free_mbuf(Mbuf* m) {
        if (m) m->in_use = false;  // Mark available (our simulation)
        // Real DPDK: rte_pktmbuf_free(m) — returns to per-lcore cache ring
    }

    size_t pool_size() const { return pool_.size(); }

private:
    // The pool storage: contiguous array of Mbuf objects.
    // In real DPDK: this is allocated from hugepages by rte_mempool_create().
    // All mbufs are physically contiguous in memory → cache-friendly access patterns.
    std::vector<Mbuf> pool_;

    // Atomic bump-pointer: next mbuf index to allocate.
    // atomic<uint32_t> ensures multiple threads don't get the same index.
    // In our single-lcore simulation: only one thread calls alloc() at a time,
    // but the atomic ensures correctness if we ever run benchmarks with multiple threads.
    std::atomic<uint32_t> next_;
};

} // namespace upf
