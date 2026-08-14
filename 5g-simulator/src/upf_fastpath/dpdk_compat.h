#pragma once
// =============================================================================
// dpdk_compat.h — Production DPDK translation layer
//
// PURPOSE
// ──────────────────────────────────────────────────────────────────────────
// This header bridges the simulation (Mac) and real DPDK (Oracle Cloud).
//
//   ENABLE_DPDK=0 (default, Mac):
//     Uses our SPSCRing / MbufPool / DpdkPath simulation classes.
//     No DPDK installed. Compiles anywhere with C++17.
//
//   ENABLE_DPDK=1 (Oracle Cloud, real Linux):
//     Replaces simulation primitives with real rte_* DPDK calls.
//     Requires: dpdk-dev, hugepages, VFIO-bound NIC.
//     See oracle_setup.sh for full setup.
//
// SIMULATION → PRODUCTION MAPPING
// ──────────────────────────────────────────────────────────────────────────
//
//  Simulation call                Real DPDK call
//  ─────────────────────────────  ──────────────────────────────────────────
//  MbufPool(65536)                rte_pktmbuf_pool_create(n, cache=512, ...)
//  MbufPool::alloc()              rte_pktmbuf_alloc(pool)
//  MbufPool::free_mbuf(m)         rte_pktmbuf_free(m)
//  SPSCRing::push(mbuf*)          [not used — NIC DMA fills mbufs directly]
//  SPSCRing::pop_burst(n)         rte_eth_rx_burst(port, queue, mbufs, n)
//  DpdkPath::start()              rte_eal_remote_launch(lcore_fn, arg, lcore_id)
//  Packet::now_ns()               rte_rdtsc() / rte_get_tsc_hz() * 1e9
//  RuleTable (unordered_map)      rte_hash (cuckoo hash, NUMA-local)
//  std::thread pinned to core     rte_lcore (isolated via --lcores EAL arg)
//
// BUILDING ON ORACLE CLOUD
// ──────────────────────────────────────────────────────────────────────────
//   # 1. Install DPDK (see oracle_setup.sh)
//   # 2. Configure hugepages
//   # 3. Bind NIC to VFIO (or use net_tap vdev for testing without real NIC)
//   # 4. Build:
//   make ENABLE_DPDK=1
//   # 5. Run (EAL args required before --):
//   sudo ./upf_benchmark -l 0-1 -n 4 --vdev net_tap0 -- --ues 100
// =============================================================================

// ─────────────────────────────────────────────────────────────────────────────
// SIMULATION MODE  (Mac, ENABLE_DPDK not set)
// ─────────────────────────────────────────────────────────────────────────────
#ifndef ENABLE_DPDK

#include "ring_buffer.h"
#include "mbuf_pool.h"
#include "dpdk_path.h"
#include "socket_path.h"
#include "stats.h"
#include "pfcp_rules.h"

namespace upf {

// dpdk_now_ns: simulation uses std::chrono (same as Packet::now_ns())
inline uint64_t dpdk_now_ns() { return Packet::now_ns(); }

} // namespace upf

// ─────────────────────────────────────────────────────────────────────────────
// REAL DPDK MODE  (Oracle Cloud, ENABLE_DPDK=1)
// ─────────────────────────────────────────────────────────────────────────────
#else  // ENABLE_DPDK

#include <cstdint>
#include <atomic>
#include <cstdlib>
#include <cstdio>

// DPDK environment abstraction layer
#include <rte_eal.h>
#include <rte_common.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_per_lcore.h>

// Memory and buffers
#include <rte_mempool.h>
#include <rte_mbuf.h>
#include <rte_malloc.h>

// NIC / ethdev
#include <rte_ethdev.h>
#include <rte_eth_ctrl.h>

// Data structures
#include <rte_ring.h>
#include <rte_hash.h>
#include <rte_jhash.h>

// Timing
#include <rte_cycles.h>
#include <rte_timer.h>

// Metrics
#include <rte_metrics.h>

#include "packet.h"
#include "pfcp_rules.h"
#include "stats.h"

namespace upf {

// =============================================================================
// Timing: rte_rdtsc() is a single x86 RDTSC instruction (~1ns overhead)
// Compare: std::chrono::high_resolution_clock::now() may call vDSO (~20ns)
// At 10Mpps, saving 19ns/call = 190ms/second of pure timestamping overhead.
// =============================================================================
inline uint64_t dpdk_now_ns() {
    // rte_get_tsc_hz() returns CPU cycles per second (calibrated at init).
    // This divides TSC ticks by Hz to get nanoseconds.
    // In production, many UPFs cache 1.0/rte_get_tsc_hz() at startup to avoid
    // the double division in the hot path.
    static const double tsc_hz = static_cast<double>(rte_get_tsc_hz());
    return static_cast<uint64_t>(
        static_cast<double>(rte_rdtsc()) / tsc_hz * 1e9
    );
}

// =============================================================================
// EAL initialization wrapper
//
// EAL (Environment Abstraction Layer) is the entry point for all DPDK apps.
// It must be called ONCE before any rte_* functions.
//
// Key EAL command-line arguments:
//   -l 0-3          : use lcores 0,1,2,3 (one per CPU core)
//   -n 4            : 4 memory channels (matches DDR channels on the board)
//   --huge-dir /mnt/huge  : where 2MB hugepages are mounted
//   --vdev net_tap0 : virtual tap device (for testing without real NIC)
//   --proc-type primary   : this process owns the ports (vs secondary spy)
//
// On Oracle Cloud A1 (ARM):
//   sudo ./upf_benchmark -l 0-1 -n 2 --vdev net_tap0 -- --ues 100
// =============================================================================
inline void init_eal(int argc, char** argv) {
    int ret = rte_eal_init(argc, argv);
    if (ret < 0) {
        rte_exit(EXIT_FAILURE, "EAL init failed: %s\n", rte_strerror(rte_errno));
    }
    // Returns the number of args consumed by EAL.
    // Your app args start at argv[ret+1].
}

// =============================================================================
// RealMbufPool — wraps rte_pktmbuf_pool_create
//
// HUGEPAGES: DPDK allocates from /mnt/huge (2MB pages).
// A pool of 65536 mbufs at 2176 bytes each = ~143MB.
// With 2MB hugepages: 72 TLB entries cover the entire pool.
// With 4KB pages: the same pool would need 36,864 TLB entries.
//
// NUMA AWARENESS: rte_eth_dev_socket_id(port) ensures the memory pool is
// allocated on the same NUMA node as the NIC. Cross-NUMA access adds ~100ns
// latency per mbuf and cuts effective memory bandwidth in half.
// =============================================================================
class RealMbufPool {
public:
    static constexpr uint32_t NUM_MBUFS      = 65536;
    static constexpr uint32_t MBUF_CACHE_SZ  = 512;   // per-lcore cache

    explicit RealMbufPool(const char* name, uint16_t port_id = 0) {
        pool_ = rte_pktmbuf_pool_create(
            name,
            NUM_MBUFS,
            MBUF_CACHE_SZ,
            0,                             // private data size per mbuf
            RTE_MBUF_DEFAULT_BUF_SIZE,     // data room = 2176 bytes (max Ethernet MTU + headroom)
            rte_eth_dev_socket_id(port_id) // NUMA node co-located with NIC
        );
        if (!pool_) {
            rte_exit(EXIT_FAILURE, "rte_pktmbuf_pool_create('%s') failed: %s\n",
                     name, rte_strerror(rte_errno));
        }
    }

    // alloc: pop from per-lcore cache ring (~5 CPU cycles on cache hit)
    // Returns nullptr if pool empty → caller must drop the packet
    rte_mbuf* alloc() { return rte_pktmbuf_alloc(pool_); }

    // free: push back to per-lcore cache ring (~5 cycles)
    // If lcore cache full → pushes to global ring (~30 cycles, still no syscall)
    void free(rte_mbuf* m) { rte_pktmbuf_free(m); }

    rte_mempool* pool() { return pool_; }

private:
    rte_mempool* pool_{nullptr};
};

// =============================================================================
// RealNicConfig — configure a DPDK-bound NIC port
//
// Prerequisites on Oracle Cloud (see oracle_setup.sh):
//   1. modprobe vfio-pci
//   2. dpdk-devbind.py --bind=vfio-pci 0000:00:03.0  (NIC PCI address)
//
// OR use a virtual tap device (no physical NIC needed):
//   sudo ./upf_benchmark -l 0-1 -n 2 --vdev net_tap0,iface=tap0 -- ...
// =============================================================================
struct RealNicConfig {
    static constexpr uint16_t RX_DESC = 1024;  // RX ring descriptors (each points to an mbuf)
    static constexpr uint16_t TX_DESC = 1024;  // TX ring descriptors

    static void configure(uint16_t port_id, rte_mempool* mp) {
        rte_eth_conf conf{};
        // RSS (Receive Side Scaling): distribute packets across multiple RX queues
        // by hashing the 5-tuple. For UPF with single lcore, we use one queue.
        conf.rxmode.mq_mode = RTE_ETH_MQ_RX_NONE;
        // Enable hardware IP/TCP/UDP checksum offload if NIC supports it
        conf.txmode.offloads = RTE_ETH_TX_OFFLOAD_IPV4_CKSUM |
                               RTE_ETH_TX_OFFLOAD_UDP_CKSUM;

        int ret = rte_eth_dev_configure(port_id, /*nb_rxq=*/1, /*nb_txq=*/1, &conf);
        if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_dev_configure failed: %d\n", ret);

        // RX queue setup: NIC DMA fills mbufs from our pool into this descriptor ring.
        // After rte_eth_dev_start(), the NIC hardware DMA engine is running.
        ret = rte_eth_rx_queue_setup(
            port_id, /*queue_id=*/0, RX_DESC,
            rte_eth_dev_socket_id(port_id),
            /*rx_conf=*/nullptr,  // use NIC default (CRC strip, buffer reuse)
            mp
        );
        if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup failed: %d\n", ret);

        ret = rte_eth_tx_queue_setup(
            port_id, 0, TX_DESC,
            rte_eth_dev_socket_id(port_id),
            nullptr
        );
        if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup failed: %d\n", ret);

        ret = rte_eth_dev_start(port_id);
        if (ret < 0) rte_exit(EXIT_FAILURE, "rte_eth_dev_start failed: %d\n", ret);

        // Promiscuous mode: accept all packets regardless of destination MAC.
        // In production UPF, you'd configure MAC filtering instead.
        rte_eth_promiscuous_enable(port_id);

        // Log NIC info
        rte_eth_dev_info info{};
        rte_eth_dev_info_get(port_id, &info);
        printf("[NIC] Port %u: driver=%s, max_rx_queues=%u, max_tx_queues=%u\n",
               port_id, info.driver_name, info.max_rx_queues, info.max_tx_queues);
    }
};

// =============================================================================
// RealHashTable — wraps rte_hash for O(1) PDR TEID lookup
//
// WHY rte_hash OVER std::unordered_map:
//   std::unordered_map uses chained buckets (pointer chase on collision) and
//   allocates from the system heap. At 10Mpps, pointer chasing on cache miss
//   costs ~100ns/packet = 10ms/s of pure lookup overhead.
//
//   rte_hash uses cuckoo hashing with SIMD acceleration (SSE4.2 crc32 on x86,
//   NEON on ARM). Keys are stored in a flat array — cache-friendly, NUMA-local.
//   Lookup cost: ~80ns including the hash computation. 2x faster than std::map.
//
// In production, one rte_hash per lookup dimension:
//   teid_table_: TEID (uint32_t) → PDR ID (uint32_t)  [UL path]
//   ueip_table_: UE IP (uint32_t) → PDR ID (uint32_t) [DL path]
// =============================================================================
class RealHashTable {
public:
    explicit RealHashTable(const char* name, uint32_t capacity, int socket_id) {
        rte_hash_parameters params{};
        params.name       = name;
        params.entries    = capacity;
        params.key_len    = sizeof(uint32_t);
        params.hash_func  = rte_jhash;    // Jenkins hash, SIMD-accelerated
        params.hash_func_init_val = 0;
        params.socket_id  = socket_id;   // NUMA-local allocation
        params.extra_flag = RTE_HASH_EXTRA_FLAGS_RW_CONCURRENCY;
        // RW_CONCURRENCY: multiple readers, one writer — matches PFCP update pattern

        ht_ = rte_hash_create(&params);
        if (!ht_) {
            rte_exit(EXIT_FAILURE, "rte_hash_create('%s') failed\n", name);
        }
    }

    ~RealHashTable() {
        if (ht_) rte_hash_free(ht_);
    }

    // lookup: O(1) cuckoo hash. Returns data pointer or nullptr on miss.
    // In the lcore hot path, this replaces our unordered_map::find().
    // Typical cost: ~80ns (hash + 1-2 cache line reads)
    int32_t lookup(uint32_t key) const {
        intptr_t val = 0;
        int ret = rte_hash_lookup_data(ht_, &key, reinterpret_cast<void**>(&val));
        return (ret >= 0) ? static_cast<int32_t>(val) : -1;
    }

    // add: called from control plane (SMF PFCP Session Establishment).
    // NOT in the hot path. Uses RW_CONCURRENCY flag for lockless reads.
    void add(uint32_t key, uint32_t value) {
        intptr_t val = static_cast<intptr_t>(value);
        int ret = rte_hash_add_key_data(ht_, &key, reinterpret_cast<void*>(val));
        if (ret < 0) {
            fprintf(stderr, "rte_hash_add_key_data failed: %d\n", ret);
        }
    }

    void del(uint32_t key) {
        rte_hash_del_key(ht_, &key);
    }

private:
    rte_hash* ht_{nullptr};
};

// =============================================================================
// RealLcoreArgs — arguments passed to the lcore function
//
// The lcore function signature must be: int fn(void* arg)
// rte_eal_remote_launch() pins it to a specific CPU core via sched_setaffinity.
// On Oracle Cloud A1, use core 1 (core 0 is for OS/interrupt handling):
//   rte_eal_remote_launch(real_lcore_fn, &args, 1);
// =============================================================================
struct RealLcoreArgs {
    uint16_t            port_id{0};
    uint16_t            queue_id{0};
    RuleTable*          rules{nullptr};
    PathStats*          stats{nullptr};
    std::atomic<bool>*  running{nullptr};
};

// =============================================================================
// real_lcore_fn — the production DPDK lcore (replaces DpdkPath::lcore_poll_loop)
//
// THIS IS THE HOT PATH. Every instruction here costs at 10-40 Mpps.
// Key differences from our simulation:
//
//   rte_eth_rx_burst()  : actual NIC polling — no SPSCRing, NIC DMA already done
//   rte_rdtsc()         : single instruction timestamp vs std::chrono vDSO call
//   rte_pktmbuf_free()  : returns mbuf to per-lcore cache ring (~5 cycles)
//   NO mutex, NO condvar, NO syscall of any kind
//
// Packet parsing: in production, the lcore parses the actual GTP-U/UDP/IP
// headers from rte_pktmbuf_mtod(m, uint8_t*). We show the structure here.
// =============================================================================
static int real_lcore_fn(void* arg) {
    auto* a = static_cast<RealLcoreArgs*>(arg);
    constexpr uint16_t BURST = 32;
    rte_mbuf* burst[BURST];

    // Typical production UPF GTP-U header offsets (N3 interface, outer UDP encap):
    //   Ethernet header : 14 bytes
    //   IP header       : 20 bytes (no options)
    //   UDP header      :  8 bytes
    //   GTP-U header    :  8 bytes (flags + msg_type + length + TEID)
    //   Total offset to TEID: 14+20+8+4 = 46 bytes (4 bytes into GTP-U header)
    constexpr int TEID_OFFSET = 46;  // bytes from start of Ethernet frame

    printf("[lcore %u] started on port %u queue %u\n",
           rte_lcore_id(), a->port_id, a->queue_id);

    while (a->running->load(std::memory_order_relaxed)) {
        // ── THE KEY DPDK CALL ─────────────────────────────────────────────
        // rte_eth_rx_burst() reads the NIC's RX descriptor ring.
        // If the NIC DMA engine has filled mbufs, n > 0. Otherwise n = 0.
        // This is NOT a syscall — it reads/writes memory-mapped NIC registers.
        // Typical cost: ~50-200ns for a full burst of 32 packets.
        uint16_t n = rte_eth_rx_burst(a->port_id, a->queue_id, burst, BURST);
        if (n == 0) {
            // Empty ring: immediately loop back (busy-poll).
            // This is the 100% CPU usage we accept for sub-microsecond latency.
            continue;
        }

        // One timestamp per burst (same as our simulation's single now_ns())
        // rte_rdtsc() compiles to a single RDTSC instruction on x86 / CNTVCT on ARM
        const uint64_t now = dpdk_now_ns();

        for (uint16_t i = 0; i < n; ++i) {
            rte_mbuf* m = burst[i];

            // ── Parse GTP-U TEID from raw packet bytes ─────────────────────
            // rte_pktmbuf_mtod: returns pointer to first byte of packet data.
            // The TEID is at offset 46 in a standard outer-UDP GTP-U frame.
            const uint8_t* data = rte_pktmbuf_mtod(m, const uint8_t*);
            if (rte_pktmbuf_data_len(m) < static_cast<uint16_t>(TEID_OFFSET + 4)) {
                // Packet too short: drop
                rte_pktmbuf_free(m);
                a->stats->pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            // Network byte order → host byte order
            uint32_t teid = (static_cast<uint32_t>(data[TEID_OFFSET])     << 24) |
                            (static_cast<uint32_t>(data[TEID_OFFSET + 1]) << 16) |
                            (static_cast<uint32_t>(data[TEID_OFFSET + 2]) <<  8) |
                            (static_cast<uint32_t>(data[TEID_OFFSET + 3]));

            // ── PDR lookup (O(1)) ──────────────────────────────────────────
            // In production, use RealHashTable::lookup(teid) here.
            // For this bridge layer, we call the same RuleTable to keep
            // the code portable. RuleTable uses unordered_map — replace with
            // RealHashTable for full production performance.
            Packet pkt{};
            pkt.teid = teid;
            pkt.dir  = Direction::UL;  // N3 ingress = UL
            pkt.arrival_ns = now;

            const PDR* pdr = a->rules->match_pdr(pkt);
            if (!pdr) {
                a->stats->pkts_no_pdr.fetch_add(1, std::memory_order_relaxed);
                a->stats->pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                rte_pktmbuf_free(m);
                continue;
            }

            // ── FAR action ────────────────────────────────────────────────
            const FAR* far = a->rules->get_far(pdr->far_id);
            if (!far || far->action == FarAction::DROP) {
                a->stats->pkts_dropped.fetch_add(1, std::memory_order_relaxed);
                rte_pktmbuf_free(m);
                continue;
            }

            // ── Forward ───────────────────────────────────────────────────
            // UL (N3 → N6): strip GTP-U outer header, send inner IP to internet
            //   rte_pktmbuf_adj(m, TEID_OFFSET + 8)  // advance past outer headers
            //   rte_eth_tx_burst(n6_port, 0, &m, 1)
            //
            // DL (N6 → N3): prepend new GTP-U outer header, send to gNB
            //   char* hdr = rte_pktmbuf_prepend(m, sizeof(gtp_outer_t))
            //   fill_gtp_header(hdr, pdr->teid, m->pkt_len - sizeof(gtp_outer_t))
            //   rte_eth_tx_burst(n3_port, 0, &m, 1)
            //
            // For this demo: just free the mbuf (simulates TX completion)
            a->stats->pkts_forwarded.fetch_add(1, std::memory_order_relaxed);
            a->stats->record_latency(now, dpdk_now_ns());

            rte_pktmbuf_free(m);  // ~5 CPU cycles: push to per-lcore cache
        }
    }
    return 0;
}

// =============================================================================
// launch_real_lcore — start the lcore and pin it to a CPU core
//
// rte_eal_remote_launch() calls sched_setaffinity() under the hood.
// The lcore runs at 100% CPU on that core.
//
// ISOLATION: For production, add isolcpus=1,2,3 to kernel boot params so the
// OS scheduler never preempts the lcore. On Oracle Cloud:
//   /etc/default/grub: GRUB_CMDLINE_LINUX="isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3"
//   sudo update-grub && sudo reboot
//
// After isolation, lcore jitter drops from ~1μs to ~50ns.
// =============================================================================
inline void launch_real_lcore(unsigned lcore_id, RealLcoreArgs* args) {
    if (!rte_lcore_is_enabled(lcore_id)) {
        rte_exit(EXIT_FAILURE, "lcore %u not enabled — check -l EAL argument\n", lcore_id);
    }
    int ret = rte_eal_remote_launch(real_lcore_fn, args, lcore_id);
    if (ret != 0) {
        rte_exit(EXIT_FAILURE, "rte_eal_remote_launch failed for lcore %u\n", lcore_id);
    }
    printf("[main] lcore %u launched\n", lcore_id);
}

// Wait for all lcores to finish (equivalent to pthread_join for all lcores)
inline void wait_all_lcores() {
    rte_eal_mp_wait_lcore();
}

} // namespace upf

#endif  // ENABLE_DPDK
