# DPDK UPF Interview Guide

A practical guide for discussing DPDK, UPF fast-path, and 5G packet forwarding in technical interviews.

---

## 1. What Is DPDK — In Plain Language

**DPDK (Data Plane Development Kit)** is a set of libraries that let a Linux application talk directly to a network interface card (NIC) — completely bypassing the Linux kernel's network stack.

Normally when a packet arrives at a NIC:
1. The NIC raises a hardware interrupt.
2. The kernel's interrupt handler fires, copies the packet into an `sk_buff`, and pushes it through the network stack (IP, UDP, socket layer).
3. Your application calls `recvfrom()` — a syscall that causes another user↔kernel mode switch — to retrieve the packet.
4. The packet has now been copied 2-3 times and involved 2+ syscalls.

With DPDK:
1. The NIC is "unbound" from its kernel driver using VFIO or UIO.
2. DPDK maps the NIC's PCI memory directly into userspace using `mmap`.
3. Your application runs a **Poll Mode Driver (PMD)**: a tight busy-loop that reads the NIC's DMA descriptor ring directly.
4. The packet goes from wire → userspace mbuf in one step, with no interrupt, no syscall, no kernel copy.

The result: **~10-100x higher throughput** and **~100x lower latency** per core, at the cost of one CPU core running at 100% even when idle.

### Key DPDK Concepts

| Concept | What It Is | Why It Matters |
|---|---|---|
| PMD (Poll Mode Driver) | Userspace NIC driver that busy-polls | Eliminates interrupt latency (~1-5μs) |
| rte_mbuf | Pre-allocated packet buffer (like sk_buff) | No malloc on hot path |
| rte_mempool | Hugepage-backed pool of mbufs | Lock-free alloc/free, TLB efficiency |
| rte_ring | Lock-free circular buffer (MPMC/SPSC) | Inter-core packet passing without mutex |
| Hugepages | 2MB or 1GB pages (vs default 4KB) | Reduces TLB misses by 512x |
| lcore | Logical core = DPDK's pinned thread | One lcore per CPU core, no migration |
| EAL | Environment Abstraction Layer | DPDK startup, core/memory allocation |
| rte_eth_rx_burst() | Burst receive from NIC | Amortizes per-batch overhead |

---

## 2. Where DPDK Fits in the 5G UPF

The UPF (User Plane Function) is the 5G Core network element that handles actual user traffic. It sits at the intersection of three N-interfaces:

```
                    ┌─────────────────────────────┐
                    │            SMF              │
                    │  (Session Management Fn)    │
                    └──────────────┬──────────────┘
                                   │ N4 (PFCP)
                                   │ "program my rules"
                    ┌──────────────▼──────────────┐
   gNB ─── N3 ────►│                             │──── N6 ────► Internet
  (GTP-U)          │    UPF (User Plane Fn)      │    (plain IP)
                   │                             │
                   │  ┌─────────────────────┐    │
                   │  │   DPDK FAST PATH    │    │
                   │  │  PDR → FAR → QER    │    │
                   │  │  rte_eth_rx_burst() │    │
                   │  │  rte_ring / mbuf    │    │
                   │  └─────────────────────┘    │
                   └─────────────────────────────┘
```

**DPDK lives exclusively in the data plane (N3 ↔ N6 forwarding).** The N4/PFCP control-plane interface is a normal Linux socket — DPDK is not used there because it carries infrequent control messages, not millions of packets per second.

### What the UPF Does Per Packet

**Uplink (UE → Internet):**
1. Receive GTP-U packet on N3 (outer: UDP/IP from gNB; inner: UE's IP packet)
2. Parse outer GTP-U header → extract TEID
3. PDR lookup: TEID → matched PDR → get FAR + QER
4. QER: apply rate limiting (token bucket, per-QFI)
5. FAR: strip GTP-U outer header, forward inner IP to N6 (internet)

**Downlink (Internet → UE):**
1. Receive plain IP packet on N6 (destination = UE's IP)
2. PDR lookup: destination UE-IP → matched PDR → get FAR + QER
3. QER: apply rate limiting
4. FAR: add GTP-U outer header (TEID from FAR, dst = gNB N3 IP:2152), forward on N3

---

## 3. The Two Paths Compared

### Socket Path (Kernel-Based UPF)

```
Wire
 │
 ▼
[NIC RX]
 │ interrupt → kernel IRQ handler
 ▼
[Kernel network stack]
 │ sk_buff alloc + GTP-U socket processing
 │ ~500-2000ns (syscall overhead alone)
 ▼
[recvfrom() syscall]
 │ user↔kernel mode switch
 │ ~500-2000ns per call
 ▼
[Application queue]
 │ mutex lock → queue push → cv.notify_one()
 │ ~20-100ns (mutex) + ~200ns-1μs (notify)
 ▼
[Worker thread wakes]
 │ condition_variable::wait() returns
 │ ~5-50μs scheduler wakeup latency
 ▼
[PDR lookup + FAR + QER]
 │ ~100-500ns
 ▼
[sendto() syscall]
 │ another user↔kernel switch
 │ ~500-2000ns
 ▼
[NIC TX]

Total per-packet: ~10μs - 100μs
Throughput:       ~200Kpps - 1Mpps per core
```

### DPDK Path (Poll-Mode UPF)

```
Wire
 │
 ▼
[NIC RX descriptors]
 │ DMA → pre-allocated hugepage mbuf (no copy, no interrupt)
 ▼
[rte_eth_rx_burst()]
 │ lcore polls this in tight loop
 │ Returns 0-32 mbufs immediately
 │ ~20-50ns per burst if non-zero
 ▼
[Burst processing (32 packets)]
 │ One rte_rdtsc() for timestamp
 │ 32x PDR hash lookup (cache warm)
 │ 32x FAR action
 │ 32x QER token bucket
 │ ~50-200ns total for 32 packets
 ▼
[rte_eth_tx_burst()]
 │ Places mbufs in TX ring
 │ NIC DMA sends them
 │ ~10-50ns
 ▼
[NIC TX]

Total per-packet: ~50-500ns (10-100x faster)
Throughput:       10-40 Mpps per core
```

### What Changes in Each Path

| Feature | Socket/Kernel Path | DPDK Path |
|---|---|---|
| Interrupt handling | Hardware interrupt → kernel IRQ | None (PMD polls) |
| Memory | sk_buff in kernel heap | rte_mbuf in hugepage mempool |
| Copy count | 2-3 copies (NIC→kernel→app) | 0-1 copies (NIC DMA → mbuf) |
| Syscalls per packet | 2 (recvfrom + sendto) | 0 |
| Synchronization | mutex + condition_variable | Lock-free ring (rte_ring) |
| CPU when idle | ~0% (thread sleeps) | 100% (busy-poll) |
| Scheduling jitter | ~5-50μs (wake latency) | <100ns (no sleep) |
| Latency | 10μs - 100μs | 50ns - 500ns |

---

## 4. What Changes in Production

### a) Hugepages Configuration

Before starting a DPDK UPF:
```bash
# 1GB hugepages (recommended for large mbuf pools)
echo 8 > /sys/kernel/mm/hugepages/hugepages-1048576kB/nr_hugepages
# 2MB hugepages (more flexible, minimum for DPDK)
echo 1024 > /sys/kernel/mm/hugepages/hugepages-2048kB/nr_hugepages
# Mount hugetlbfs
mount -t hugetlbfs hugetlbfs /dev/hugepages
```

### b) VFIO/UIO Binding

```bash
# Load VFIO driver
modprobe vfio-pci

# Find NIC's PCI address
dpdk-devbind.py --status

# Unbind from kernel driver, bind to vfio-pci
dpdk-devbind.py --unbind 0000:01:00.0
dpdk-devbind.py --bind vfio-pci 0000:01:00.0
```

### c) NUMA Pinning

In a dual-socket server, the UPF lcore should run on the same NUMA node as the NIC:
```c
// In DPDK app:
int socket_id = rte_eth_dev_socket_id(port_id);
struct rte_mempool *pool = rte_pktmbuf_pool_create(
    "MBUF_POOL", NUM_MBUFS, MBUF_CACHE_SIZE, 0,
    RTE_MBUF_DEFAULT_BUF_SIZE, socket_id);
```
Cross-NUMA access adds ~100ns latency and halves memory bandwidth.

### d) PFCP Rule Updates (Runtime)

When the SMF sends a PFCP Session Modification Request (e.g., on handover, QoS change):
- The control plane thread receives the PFCP message on a normal Linux socket
- It must update the PDR/FAR tables without stalling the lcore
- Production approach: **RCU (Read-Copy-Update)** or per-lcore rule cache with epoch/version update
- The lcore completes the current burst, checks version number, updates its local cache

### e) Observability

Production UPFs must export:
- Per-UE/per-QFI octets and packet counts (for charging and QoS monitoring)
- PFCP session stats sent back to SMF in Usage Reports
- Prometheus metrics: `upf_rx_packets_total`, `upf_tx_packets_total`, `upf_dropped_total`, `upf_latency_ns_p99`
- DPDK ethdev stats: `rte_eth_stats_get()` for NIC-level counters

---

## 5. How to Talk About This in an Interview

### Sample Answer Script

**Q: "What is DPDK and how is it used in a 5G UPF?"**

> "DPDK stands for Data Plane Development Kit. In a normal Linux kernel UPF, every packet goes through interrupt handling, kernel network stack, and two syscalls — recvfrom and sendto. That caps throughput at maybe 200K to 1M packets per second per core, with 10 to 100 microsecond latency.
>
> DPDK bypasses all of that. The NIC is bound to a VFIO driver, which maps it directly into userspace. We run a Poll Mode Driver — a tight busy-loop that calls rte_eth_rx_burst() instead of recvfrom. No interrupts, no syscalls, no mutex. Packets go from wire to application in one DMA copy into pre-allocated hugepage-backed mbufs.
>
> In the 5G UPF specifically, DPDK handles the N3-to-N6 forwarding: receiving GTP-U packets from the gNB, doing PDR/FAR/QER lookup using hash tables, and forwarding to the internet or back to the gNB. The control plane — N4 PFCP — still uses normal Linux sockets because it's just a few messages per second.
>
> The trade-off is: you burn one CPU core at 100% even when traffic is zero, because the lcore never sleeps. For a carrier-grade UPF that needs to meet 5QI=1 voice latency SLAs, that's an acceptable trade."

**Q: "Have you implemented this?"**

> "I built a learning simulator in C++ that models both paths — a socket-based path using mutex and condition_variable (like a kernel UPF) and a DPDK-style path using a lock-free SPSC ring and burst processing (like OAI-UPF or free5GC with DPDK). The benchmark shows the DPDK path is [X]x faster on my machine. It doesn't run real DPDK — I'm modeling the architectural difference, not running against a real NIC. But I can walk through the code and explain every overhead source."

### What to Volunteer Proactively

- "I understand this is a simulation — I haven't run real DPDK against a Mellanox NIC"
- "In production you'd also need hugepages, VFIO binding, and NUMA pinning"
- "Real UPFs like OAI-UPF use VPP which has similar architecture"
- "The PDR/FAR tables need RCU-style updates so the lcore never stalls"

---

## 6. What Our Simulator Models vs. What It Simplifies

### What We Model Accurately

| Feature | How |
|---|---|
| Lock-free SPSC ring | `SPSCRing<T,N>` with correct acquire/release atomics |
| Burst processing | `pop_burst()` + single `now_ns()` per burst |
| Pre-allocated pool | `MbufPool` with bump-pointer alloc |
| PFCP data model | `PDR`, `FAR`, `QER` structs matching 3GPP TS 29.244 |
| O(1) PDR lookup | `teid_to_pdr_` and `ueip_to_pdr_` hash maps |
| Mutex+CV overhead | `SocketPath` with real `std::mutex` + `condition_variable` |
| Cache-line padding | `alignas(64)` on ring head/tail |
| Latency measurement | Reservoir sampling, p50/p95/p99 |

### What We Simplify

| Feature | What We Skip | Production Reality |
|---|---|---|
| Zero-copy DMA | We memcpy into mbuf | NIC DMA direct to mbuf, no CPU copy |
| Hugepages | `std::vector` (heap) | 2MB/1GB pages, `/dev/hugepages` |
| Real NIC PMD | SPSCRing | rte_eth_rx_burst() with mlx5/i40e PMD |
| Multi-queue RSS | Single queue | NIC hashes flows to multiple lcores |
| Token-bucket QER | We lookup but don't enforce | rte_meter_trtcm per QFI |
| GTP-U parsing | We use pre-parsed TEID/UE-IP | Real parsing of outer+inner IP headers |
| TX burst | We "forward" by incrementing counter | rte_eth_tx_burst() + TX ring |
| RCU rule updates | Rules are immutable after setup | Read-Copy-Update for live PFCP updates |
| NUMA awareness | Single socket assumed | rte_socket_id() + per-NUMA pools |
