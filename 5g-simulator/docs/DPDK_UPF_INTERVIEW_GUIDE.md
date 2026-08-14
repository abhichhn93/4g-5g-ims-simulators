# DPDK UPF Interview Guide

A practical guide for discussing DPDK, UPF fast-path, and 5G packet forwarding in technical interviews.

**Interactive visual guides (open in browser):**
- DPDK Concepts + Interview Q&A: https://claude.ai/code/artifact/ada986a3-b413-4f5d-9e92-e917d8c78e8f
- DPDK Packet Flow + Ring Buffer Diagram + Oracle Cloud setup: https://claude.ai/code/artifact/a2b8705d-39e1-4d9e-82e0-3dfcc2de1e50

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

---

## 7. End-to-End Packet Journey — Plain Language with Diagrams

> This section explains the full life of a packet — from a UE sending a YouTube
> request all the way through the UPF and back. Every function is explained in
> plain terms, compared to its Linux equivalent, and tied to what it is actually
> doing with the data.

---

### 7.1 The Big Picture — Two Directions

```
╔══════════════════════════════════════════════════════════════════════════════╗
║                    THE TWO PACKET DIRECTIONS                                 ║
║                                                                              ║
║  UPLINK  (UE → Internet)                                                     ║
║  ─────────────────────────────────────────────────────────────────────────   ║
║                                                                              ║
║   [UE Phone]                                                                 ║
║       │  sends YouTube request (plain IP packet: src=10.45.0.2)             ║
║       │  over the radio (LTE/5G air interface)                               ║
║       ▼                                                                      ║
║   [gNB — base station]                                                       ║
║       │  wraps the UE packet in a GTP-U "envelope"                           ║
║       │  Outer packet: gNB IP → UPF IP, UDP port 2152                        ║
║       │  Inside the envelope: TEID=1001 (identifies this UE's session)       ║
║       │  then the UE's original IP packet                                    ║
║       │  ──────────────────── N3 interface ────────────────────────────────  ║
║       ▼                                                                      ║
║   [UPF]                                                                      ║
║       │  1. Receive the GTP-U packet on N3                                   ║
║       │  2. Open the envelope → read TEID=1001                               ║
║       │  3. PDR lookup: "who owns TEID 1001?" → UE-A                         ║
║       │  4. QER check: "is UE-A sending too fast?" → OK                      ║
║       │  5. FAR action: "strip the GTP-U wrapper, forward inner IP"          ║
║       │  ──────────────────── N6 interface ────────────────────────────────  ║
║       ▼                                                                      ║
║   [Internet] ← bare IP packet (src=10.45.0.2 or NATed to UPF's public IP)   ║
║                                                                              ║
║  DOWNLINK  (Internet → UE)                                                   ║
║  ─────────────────────────────────────────────────────────────────────────   ║
║                                                                              ║
║   [YouTube server] → sends reply packet → dest = UE's IP (10.45.0.2)        ║
║       │  ──────────────────── N6 interface ────────────────────────────────  ║
║       ▼                                                                      ║
║   [UPF]                                                                      ║
║       │  1. Receive plain IP packet on N6 (dest = 10.45.0.2)                 ║
║       │  2. PDR lookup: "who has IP 10.45.0.2?" → UE-A                       ║
║       │  3. QER check: "is the download rate OK?" → OK                       ║
║       │  4. FAR action: "wrap in GTP-U, send to gNB"                         ║
║       │     Add GTP-U header: TEID=2001, dst = gNB's N3 IP                   ║
║       │  ──────────────────── N3 interface ────────────────────────────────  ║
║       ▼                                                                      ║
║   [gNB] → strips GTP-U → sends radio packet to UE                            ║
║       ▼                                                                      ║
║   [UE Phone] ← receives YouTube video                                        ║
╚══════════════════════════════════════════════════════════════════════════════╝
```

---

### 7.2 What PDR, FAR, and QER Actually Mean — No Jargon

Think of the UPF as a post office sorting room. Every packet that arrives is a parcel.

**PDR = "Who is this parcel for?" (the label reader)**

When a GTP-U packet arrives on N3, it has a number stamped on it: the TEID.
The PDR table is simply a dictionary:

```
TEID 1001  →  this is UE-A's uplink session, use QER#1, use FAR#1
TEID 1002  →  this is UE-B's uplink session, use QER#2, use FAR#1
IP 10.45.0.2  →  this is UE-A's downlink, use QER#1, use FAR#2
IP 10.45.0.3  →  this is UE-B's downlink, use QER#2, use FAR#2
```

In our code (`pfcp_rules.h`):
```cpp
// The PDR table is just two hash maps:
std::unordered_map<uint32_t, uint32_t> teid_to_pdr_;   // UL: TEID → PDR ID
std::unordered_map<uint32_t, uint32_t> ueip_to_pdr_;   // DL: UE IP → PDR ID

// Looking up a UL packet:
const PDR* match_pdr(const Packet& p) {
    if (p.dir == Direction::UL)
        return lookup by p.teid    // O(1) hash lookup
    else
        return lookup by p.ue_ip   // O(1) hash lookup
}
```

**QER = "Is this UE sending/receiving too fast?" (the speed camera)**

After finding the PDR, we check the QER (Quality of Service Enforcement Rule).
It says: "This UE is on a 10 Mbps data plan. If they're sending 100 Mbps, drop packets."

Real production uses a token-bucket algorithm (tokens fill at 10Mbps rate; each packet
spends tokens; empty bucket = drop). In our simulator we look up the QER but don't
enforce it — we model the structure, not the policing.

```cpp
struct QER {
    uint32_t qer_id;
    uint8_t  qfi;          // QoS Flow Identifier (1=voice, 2=video, 9=data)
    uint32_t ul_mbr_kbps;  // max UL rate (e.g., 10000 = 10 Mbps)
    uint32_t dl_mbr_kbps;  // max DL rate
    uint32_t ul_gbr_kbps;  // guaranteed UL rate (voice/video only)
    uint32_t dl_gbr_kbps;  // guaranteed DL rate
};
```

QFI values you'll be asked about:
- `QFI=1` → voice call (VoNR) — guaranteed 128 Kbps, must not be dropped
- `QFI=2` → video call — guaranteed rate
- `QFI=9` → normal data (YouTube, web) — best effort, can be dropped if congested

**FAR = "Where does this packet go and how?" (the routing label)**

After QER check, the FAR says what to do:

```cpp
enum class FarAction { FORWARD, DROP, BUFFER };

struct FAR {
    uint32_t   far_id;
    FarAction  action;      // what to do
    uint32_t   dst_ip;      // where to send (gNB IP for DL, internet gateway for UL)
    uint16_t   dst_port;    // usually 2152 (GTP-U port) for DL
    bool       encap_gtpu;  // DL=true (add GTP-U wrapper), UL=false (strip it)
};
```

- **UL FAR**: action=FORWARD, encap_gtpu=false → strip the outer GTP-U header,
  send the inner IP packet to N6 (internet)
- **DL FAR**: action=FORWARD, encap_gtpu=true → wrap in GTP-U with the DL TEID,
  send to gNB on N3
- **BUFFER**: used during handover — hold packets while UE moves between gNBs

**The three-step pipeline, every packet:**
```
Packet arrives
    │
    ▼
[PDR] → "who is this?" → found PDR { qer_id=1, far_id=1 }
    │
    ▼
[QER] → "are they within their speed limit?" → OK / DROP
    │
    ▼
[FAR] → "what to do?" → FORWARD to internet (UL) or FORWARD to gNB (DL)
```

---

### 7.3 Linux Socket Path — Function by Function

This is what a basic kernel-based UPF does. It's simple but slow.

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    LINUX SOCKET PATH — UL PACKET                        │
│                                                                         │
│  gNB sends GTP-U packet on UDP port 2152                                │
│                                                                         │
│  ① recvfrom(sock_fd, buf, BUF_SIZE, 0, &addr, &addrlen)                │
│    ─────────────────────────────────────────────────────────            │
│    "Give me the next packet from the socket"                            │
│    What it does: crosses from userspace → kernel (syscall)              │
│    Kernel copies packet from sk_buff → your buf                         │
│    Cost: ~500-2000 ns just for the crossing                             │
│    If no packet arrived yet: BLOCKS here until one does                 │
│    Linux equivalent you know: exactly recvfrom() on a UDP socket        │
│                                                                         │
│  ② Parse GTP-U header manually                                          │
│    ─────────────────────────────────────────────────────────            │
│    uint32_t teid = ntohl(*(uint32_t*)(buf + GTP_TEID_OFFSET))           │
│    "Read TEID from byte offset 12 in the GTP-U header"                  │
│    Just pointer arithmetic — fast, no function call needed              │
│                                                                         │
│  ③ PDR lookup: teid_to_pdr_[teid]                                       │
│    ─────────────────────────────────────────────────────────            │
│    Hash map lookup — O(1), ~50-100 ns                                   │
│    Returns the PDR struct → get qer_id and far_id                       │
│                                                                         │
│  ④ QER check (token bucket, not in our sim but conceptually here)       │
│    ─────────────────────────────────────────────────────────            │
│    Is this UE within their MBR? If not, drop packet                     │
│                                                                         │
│  ⑤ FAR action: strip GTP-U                                              │
│    ─────────────────────────────────────────────────────────            │
│    inner_pkt_ptr = buf + GTP_HEADER_SIZE                                │
│    inner_pkt_len = received_len - GTP_HEADER_SIZE                       │
│    "Skip past the outer headers, point to the UE's original packet"     │
│                                                                         │
│  ⑥ sendto(n6_sock_fd, inner_pkt_ptr, inner_pkt_len, 0, ...)            │
│    ─────────────────────────────────────────────────────────            │
│    "Send the inner IP packet out to the internet (N6 interface)"        │
│    Another syscall → kernel copies from buf → sk_buff → NIC             │
│    Cost: ~500-2000 ns again                                             │
│                                                                         │
│  TOTAL: ~10,000 - 100,000 ns per packet                                 │
│  WHY SO SLOW: two syscalls, one kernel copy in, one kernel copy out     │
│  PLUS: if no packet, thread sleeps. Waking from sleep = ~5,000-50,000ns │
└─────────────────────────────────────────────────────────────────────────┘
```

In our simulator's `socket_path.h`, the "sleep" is done via `condition_variable::wait()`.
When you call `cv_.wait(lock, ...)`, your thread parks until someone calls `cv_.notify_one()`.
That wakeup latency is exactly what the benchmark measures (~327,000 ns at p50).

---

### 7.4 DPDK Path — Every Function Explained Simply

DPDK has two phases: **startup setup** (done once) and the **hot loop** (runs forever).

#### Setup Phase — "Getting DPDK Ready" (done once at program start)

```cpp
// ─── FUNCTION 1 ────────────────────────────────────────────────────────────
rte_eal_init(argc, argv)
// ───────────────────────────────────────────────────────────────────────────
// Plain English: "Start the DPDK engine"
// What it does:
//   - Reads hugepage config (/dev/hugepages)
//   - Pins each lcore thread to a specific CPU core (no migration)
//   - Finds the NIC (via VFIO/UIO binding)
//   - Sets up memory for all mbufs
// Linux equivalent: nothing direct. Closest is mlockall() + pthread_setaffinity_np()
// Why needed: DPDK needs physical memory locked in place so the NIC's DMA
//             engine can write directly to it. Regular malloc() memory can be
//             swapped out — DMA would then write to the wrong physical address.

// ─── FUNCTION 2 ────────────────────────────────────────────────────────────
rte_pktmbuf_pool_create("MBUF_POOL", 65536, 256, 0, 2048, socket_id)
// ───────────────────────────────────────────────────────────────────────────
// Plain English: "Pre-allocate 65,536 packet buffers right now"
// What it does: allocates a big chunk of hugepage memory, cuts it into
//               65,536 fixed-size slots (mbufs), each 2048 bytes.
//               Builds a lock-free free-list so the NIC's DMA can grab one.
// Linux equivalent: like doing malloc() for 65,536 sk_buffs upfront so
//                   the kernel never needs to malloc during packet handling.
// Why 65,536: if you receive 32 packets per burst at 10 Mpps, you process
//             them in 3.2 μs. You need enough buffer to hold packets while
//             previous bursts are being processed. 65K is safe headroom.
// In our simulator: MbufPool{65536} in dpdk_path.h does the same idea —
//                   allocate all memory upfront, use bump-pointer alloc.

// ─── FUNCTION 3 ────────────────────────────────────────────────────────────
rte_eth_dev_configure(port_id, n_rx_queues=1, n_tx_queues=1, &port_conf)
// ───────────────────────────────────────────────────────────────────────────
// Plain English: "Tell the NIC: I want 1 RX queue and 1 TX queue"
// What it does: configures the NIC hardware — how many queues, what RSS hash,
//               promiscuous mode on/off.
// Linux equivalent: like calling setsockopt() to configure a socket, but for
//                   the NIC itself.

// ─── FUNCTION 4 ────────────────────────────────────────────────────────────
rte_eth_rx_queue_setup(port_id, queue_id=0, nb_desc=512, socket_id, NULL, mbuf_pool)
// ───────────────────────────────────────────────────────────────────────────
// Plain English: "Connect the NIC's RX queue to our mbuf pool"
// What it does: sets up a ring of 512 descriptor slots. Each slot is a pointer
//               to an mbuf from our pool. The NIC fills them with incoming packets
//               via DMA. When full, it wraps around.
// Linux equivalent: nothing — kernel does this behind the scenes when you
//                   call bind() on a socket. With DPDK you control it explicitly.
// Why 512 descriptors: the NIC can receive up to 512 packets before you must
//                      call rx_burst to drain them. At 10 Mpps × 512 = ~51 μs
//                      budget. Your poll loop must call rx_burst faster than this.

// ─── FUNCTION 5 ────────────────────────────────────────────────────────────
rte_eth_dev_start(port_id)
// ───────────────────────────────────────────────────────────────────────────
// Plain English: "Turn on the NIC — start receiving packets"
// Linux equivalent: bind() + listen() (sort of). After this, the NIC hardware
//                   starts DMAing incoming packets into your mbufs.
```

#### Hot Path — "The Tight Loop That Runs Forever"

This is the part that actually processes packets. It never sleeps. It never waits.

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                  DPDK HOT LOOP — runs on one CPU core forever               │
│                                                                             │
│  while (running) {                                                          │
│                                                                             │
│  ① rte_eth_rx_burst(port_id, queue_id, mbufs[], burst_size=32)             │
│    ──────────────────────────────────────────────────────────               │
│    Plain English: "Check the NIC ring. Got any packets? Give me up to 32." │
│    Returns: 0 if nothing there, or 1-32 (number of packets received)        │
│    What it does: reads the NIC's RX descriptor ring tail pointer            │
│                  copies mbuf pointers for each filled descriptor            │
│                  advances the ring head so NIC can reuse those slots        │
│    NO SYSCALL. NO INTERRUPT. NO COPY.                                       │
│    The NIC already DMA'd packet bytes into the mbufs before this call.      │
│    This function just picks up the pre-filled envelopes.                    │
│                                                                             │
│    Linux equivalent: recvfrom() — but recvfrom() BLOCKS if empty.           │
│    DPDK equivalent: returns 0 immediately if empty. You keep looping.       │
│                                                                             │
│    Cost: ~20-50 ns whether you got packets or not                           │
│                                                                             │
│  if (n_rx == 0) continue;   ← nothing arrived, loop again immediately      │
│                                                                             │
│  ② uint64_t now = rte_rdtsc()   (called ONCE for all 32 packets)           │
│    ──────────────────────────────────────────────────────────               │
│    Plain English: "What time is it right now? (in CPU clock cycles)"        │
│    rte_rdtsc() reads the CPU's TSC (Time Stamp Counter) — one instruction.  │
│    We call it once per burst, not per packet. This is the timestamp         │
│    we stamp on all 32 packets in this burst.                                │
│    Linux equivalent: clock_gettime(CLOCK_MONOTONIC) but 10x cheaper.        │
│                                                                             │
│  ③ For each mbuf in the burst (inner loop over 1-32 packets):              │
│                                                                             │
│    rte_pktmbuf_mtod(mbuf, uint8_t*)                                         │
│    ──────────────────────────────────────────────────────────               │
│    Plain English: "Give me a pointer to the start of this packet's bytes"   │
│    It's literally: return (uint8_t*)(mbuf->buf_addr + mbuf->data_off)       │
│    Like: uint8_t* pkt = (uint8_t*)mbuf + some_offset                        │
│    Then you do pointer arithmetic: pkt + ETH_HDR_SIZE + IP_HDR_SIZE + ...   │
│    to reach the GTP-U TEID field.                                           │
│    No function call for parsing — just pointer math.                        │
│                                                                             │
│    Parse TEID / UE-IP from the packet bytes                                 │
│    ──────────────────────────────────────────────────────────               │
│    uint32_t teid = ntohl(*(uint32_t*)(pkt + ETH+IP+UDP+GTP_OFFSET))        │
│    Same as you'd do on a raw socket in C++. No magic.                       │
│                                                                             │
│    PDR lookup → QER check → FAR action (same hash maps as socket path)      │
│    ──────────────────────────────────────────────────────────               │
│    teid_to_pdr_[teid] → PDR → QER → FAR                                     │
│    Identical logic to the socket path. The SPEED difference is NOT here.    │
│    The speed difference is in rx_burst vs recvfrom and tx_burst vs sendto.  │
│                                                                             │
│    FAR: strip GTP-U header (UL) → adjust mbuf data pointer                 │
│    rte_pktmbuf_adj(mbuf, gtp_outer_size)                                    │
│    ──────────────────────────────────────────────────────────               │
│    Plain English: "Skip past the GTP-U wrapper — no copy, just move         │
│    the 'start of data' pointer forward by gtp_outer_size bytes"             │
│    Result: the mbuf now "looks like" a plain IP packet from UE              │
│    No memcpy. Just: mbuf->data_off += gtp_outer_size                        │
│                                                                             │
│    FAR: add GTP-U header (DL) → prepend to mbuf                            │
│    rte_pktmbuf_prepend(mbuf, gtp_outer_size)                                │
│    ──────────────────────────────────────────────────────────               │
│    Plain English: "Add GTP-U header in front of the packet — no copy,       │
│    just move the 'start of data' pointer backward and fill in the header"   │
│    Result: the mbuf now has the GTP-U outer wrapper prepended               │
│                                                                             │
│  ④ rte_eth_tx_burst(port_id, queue_id, mbufs[], n_to_send)                 │
│    ──────────────────────────────────────────────────────────               │
│    Plain English: "Put these packets onto the NIC's TX ring for sending"    │
│    What it does: writes mbuf pointers into the TX descriptor ring            │
│                  NIC DMA reads from the mbuf memory and sends on the wire   │
│    NO SYSCALL. NO COPY.                                                     │
│    The NIC reads directly from the hugepage memory where your mbufs live.   │
│    Linux equivalent: sendto() — but sendto() copies bytes into kernel,       │
│    DPDK sends directly from your memory.                                    │
│    Cost: ~10-50 ns for a burst of 32                                        │
│                                                                             │
│  ⑤ rte_pktmbuf_free(mbuf)   — for dropped packets                          │
│    ──────────────────────────────────────────────────────────               │
│    Plain English: "Return this packet buffer to the pool for reuse"          │
│    Lock-free: just pushes the mbuf pointer back onto the free-list.         │
│    ~5-10 ns.                                                                │
│    Linux equivalent: kfree_skb() (kernel internal). Application doesn't     │
│    free socket buffers — kernel does it automatically after sendto.         │
│                                                                             │
│  } // end while — repeat immediately, no sleep                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

### 7.5 Function Comparison Table — DPDK vs Linux

```
┌─────────────────────────┬─────────────────────────┬────────────────────────────────────────────┐
│  What you want to do    │  Linux/Socket way        │  DPDK way                                  │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Start the network layer │ socket() + bind()        │ rte_eal_init()                             │
│                         │ "set up a socket"        │ "set up the whole DPDK engine, bind NIC    │
│                         │                          │  to userspace, allocate hugepages"         │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Pre-allocate buffers    │ (kernel does this for    │ rte_pktmbuf_pool_create()                  │
│                         │  you, you don't control  │ "create a pool of 65536 packet buffers     │
│                         │  it)                     │  right now, in hugepage memory"            │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Receive a packet        │ recvfrom(fd, buf, ...)   │ rte_eth_rx_burst(port, q, mbufs, 32)       │
│                         │ - blocks if no packet    │ - returns 0 immediately if no packet       │
│                         │ - syscall (~500-2000ns)  │ - no syscall (~20-50ns)                    │
│                         │ - kernel copies bytes    │ - NIC already DMA'd bytes into mbufs       │
│                         │   into your buffer       │   before this call                         │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Get pointer to packet   │ buf is already filled    │ rte_pktmbuf_mtod(mbuf, uint8_t*)           │
│ data                    │ by recvfrom              │ "give me start pointer of this mbuf's data"│
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Strip outer header      │ pkt_ptr += header_size   │ rte_pktmbuf_adj(mbuf, header_size)         │
│ (remove GTP-U wrapper)  │ (pointer arithmetic)     │ same idea but updates mbuf metadata too    │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Add outer header        │ memmove + copy header    │ rte_pktmbuf_prepend(mbuf, header_size)     │
│ (add GTP-U wrapper)     │ into new buffer          │ moves data_off backward, no copy of body  │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Send a packet           │ sendto(fd, buf, ...)     │ rte_eth_tx_burst(port, q, mbufs, n)        │
│                         │ - syscall (~500-2000ns)  │ - no syscall (~10-50ns for 32 packets)     │
│                         │ - kernel copies your buf │ - NIC DMA reads directly from your mbufs  │
│                         │   into sk_buff → NIC     │   no intermediate copy                     │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Free a packet buffer    │ (kernel does it for you  │ rte_pktmbuf_free(mbuf)                     │
│                         │  after sendto returns)   │ returns mbuf to pool, lock-free, ~5ns      │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Get current timestamp   │ clock_gettime()          │ rte_rdtsc()                                │
│                         │ ~50-100ns, syscall       │ ~5ns, single CPU instruction               │
│                         │                          │ reads CPU's Time Stamp Counter directly    │
├─────────────────────────┼─────────────────────────┼────────────────────────────────────────────┤
│ Wait for next packet    │ epoll_wait() / select()  │ (nothing — just loop back and call         │
│                         │ sleeps until packet      │  rx_burst again. No sleep, no wait.)       │
│                         │ wakeup: ~5,000-50,000ns  │ wakeup: 0ns (you never slept)              │
└─────────────────────────┴─────────────────────────┴────────────────────────────────────────────┘
```

---

### 7.6 Why Is DPDK So Much Faster — The Root Causes

```
┌─────────────────────────────────────────────────────────────────────────────┐
│               WHERE THE TIME GOES IN THE SOCKET PATH                        │
│                                                                             │
│  100% of a packet's latency breakdown (10-100 μs total):                   │
│                                                                             │
│  ████████████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  ~50%  Sleep wakeup     │
│  │  condition_variable::wait() latency                                     │
│  │  Thread was asleep. OS scheduler must wake it.                          │
│  │  ~5,000 - 50,000 ns                                                     │
│  │                                                                         │
│  ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  ~25%  recvfrom syscall │
│  │  User → Kernel mode switch                                              │
│  │  Kernel copies sk_buff → your buffer                                    │
│  │  ~500 - 2,000 ns                                                        │
│  │                                                                         │
│  ████████░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  ~25%  sendto syscall   │
│  │  User → Kernel mode switch                                              │
│  │  Kernel copies your buffer → sk_buff → NIC ring                        │
│  │  ~500 - 2,000 ns                                                        │
│  │                                                                         │
│  ░░  <1%  PDR/FAR/QER lookup (the actual 5G logic)                         │
│       ~100-500 ns — this is the same in both paths!                        │
│                                                                             │
│  The 5G protocol logic itself is fast. The overhead is all in               │
│  syscalls, copies, and sleep/wakeup cycles.                                 │
│  DPDK eliminates all three of those.                                        │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│               WHERE THE TIME GOES IN THE DPDK PATH                          │
│                                                                             │
│  100% of a packet's latency (50-500 ns total):                              │
│                                                                             │
│  ████████████████████████████████████████████████  ~90%  PDR/FAR/QER logic │
│  │  The actual work: hash lookups, rule checks, header strip/add           │
│  │  ~50 - 450 ns                                                           │
│  │                                                                         │
│  ██  ~8%   rte_eth_rx_burst overhead (reading descriptor ring)             │
│  │   ~5 - 30 ns                                                            │
│  │                                                                         │
│  █   ~2%   rte_eth_tx_burst overhead (writing TX ring)                     │
│       ~2 - 20 ns                                                           │
│                                                                             │
│  Zero time on: syscalls, copies, sleep/wakeup.                              │
│  The CPU spends 100% of its time doing real work.                           │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

### 7.7 In Our Simulator — Which Functions Map to Which

Our simulator doesn't run real DPDK (no NIC, no hugepages). But every concept
is modeled. Here is the mapping:

```
Real DPDK function              Our simulator equivalent
──────────────────────────────  ──────────────────────────────────────────────
rte_pktmbuf_pool_create()   →   MbufPool mbuf_pool_{65536}  (dpdk_path.h)
                                Pre-allocates 65536 Mbuf structs

rte_eth_rx_burst()          →   rx_ring_.pop_burst(mbufs, burst_size)  (dpdk_path.h)
                                Lock-free SPSC ring pop, returns 0-32 items
                                No blocking, no mutex

rte_rdtsc()                 →   Packet::now_ns() called ONCE per burst
                                (dpdk_path.h line: "called outside inner loop")

PDR/FAR/QER processing      →   DpdkPath::process_burst() (dpdk_path.h)
                                Identical logic to socket path

rte_eth_tx_burst()          →   stats_.pkts_forwarded++ (simplified)
                                We count forwarded packets instead of
                                actually sending (no NIC to send to)

rte_pktmbuf_free()          →   mbuf_pool_.free_mbuf(m)  (dpdk_path.h)
                                Returns Mbuf struct to pool

sleep/wakeup (kernel side)  →   SocketPath: cv_.wait() (socket_path.h)
                                The condition_variable is the "kernel sleep"
                                we're showing is eliminated by DPDK
```

---

### 7.8 What to Say in an Interview — End-to-End Answer

**Q: "Walk me through what happens when a UE sends a packet and how DPDK fits in."**

> "Sure. The UE sends a packet — say a YouTube request. The gNB receives it over radio
> and wraps it in a GTP-U tunnel: outer IP header (gNB→UPF), UDP port 2152, a GTP-U
> header with a TEID — the tunnel identifier — and inside that is the UE's actual IP
> packet.
>
> That wrapped packet arrives at the UPF on the N3 interface.
>
> In a kernel-based UPF, the UPF calls recvfrom() — which is a syscall, so the CPU
> crosses from user mode to kernel mode, the kernel copies the packet from its sk_buff
> into your application buffer, then you return. Two syscalls per packet, two copies,
> and if no packet was ready, your thread was sleeping and just woke up — that alone
> costs 5 to 50 microseconds.
>
> With DPDK, you bypass all of that. The NIC is bound directly to userspace via VFIO.
> The application runs a poll loop calling rte_eth_rx_burst() — which just checks
> whether the NIC has filled any pre-allocated buffers and hands you pointers to them.
> No syscall, no copy, no wakeup. The NIC's DMA engine already wrote the packet bytes
> into your hugepage-backed mbufs before you even called the function.
>
> Once you have the packet, the processing — PDR lookup by TEID, QER rate check,
> FAR action (strip GTP-U, forward inner IP to N6) — is identical in both paths.
> That part is just hash map lookups, ~100-500 nanoseconds.
>
> Then rte_eth_tx_burst() puts the packet on the TX ring. Again no syscall — NIC DMA
> reads directly from your memory and sends it.
>
> The result: kernel path does ~10-100 microseconds per packet; DPDK path does
> 50-500 nanoseconds. About a 100x difference. Our benchmark showed p50 latency of
> 327,000 ns for the socket path versus 167 ns for the DPDK-style path."

---

## 8. How to Actually Learn DPDK — 3 Concepts, Mapped to Our Code

> **One rule:** Don't try to learn every DPDK function. Learn 3 concepts deeply,
> map each one to the file you already have, run the benchmark once, and you can
> talk about this for 20 minutes in any interview.

---

### Concept 1 — Kernel Network Stack vs DPDK Data Path

**What it means in one sentence:**
Normal path = your packet travels through the Linux kernel before your app sees it.
DPDK path = your app talks directly to the NIC, kernel never sees the packet.

```
NORMAL PATH (what socket_path.h models):

  Wire → NIC → [KERNEL: interrupt handler → sk_buff alloc →
  TCP/IP stack → socket buffer] → your app calls recvfrom()
  ↑ kernel is in the middle of everything

DPDK PATH (what dpdk_path.h models):

  Wire → NIC → [DMA into your hugepage mbuf] → your app
  calls rte_eth_rx_burst() → done
  ↑ kernel is completely skipped
```

**Where this is in YOUR code:**

`socket_path.h` — the kernel path:
```cpp
// This queue represents the kernel's socket buffer.
// The mutex + condition_variable = kernel's internal
// locking and wakeup mechanism made visible.
std::queue<Packet> queue_;
std::mutex mtx_;
std::condition_variable cv_;

// enqueue() = what the kernel does when a packet arrives
// worker_loop() = what your app does when it calls recvfrom()
```

`dpdk_path.h` — the DPDK path:
```cpp
// SPSCRing = the NIC's DMA descriptor ring.
// No kernel. No mutex. Just a ring of pointers.
SPSCRing<Mbuf*, RING_SIZE> rx_ring_;

// pop_burst() = rte_eth_rx_burst()
// Returns 0-32 packets immediately, no sleep, no syscall.
size_t n = rx_ring_.pop_burst(burst, BURST_SIZE);
```

**The key thing to say:**
> "The kernel path is not slow because your code is slow. It's slow because
> the kernel is doing useful things — security checks, routing decisions,
> protocol validation — that you don't need when you're a UPF that already
> knows what to do with every packet."

---

### Concept 2 — recvfrom/sendto Syscalls vs Userspace Bursts

**What a syscall costs:**
Every time you call `recvfrom()` or `sendto()`, the CPU switches from user mode
to kernel mode and back. That crossing costs ~500-2000 ns by itself — before any
packet processing happens.

```
recvfrom() timeline:

  your code          CPU mode switch         kernel
  ──────────         ─────────────           ──────
  recvfrom()  ──────► user → kernel ──────► copy sk_buff
                                            into your buf
              ◄────── kernel → user ◄──────
  you have packet
  ~500-2000 ns just for the crossing
  PLUS: if no packet ready, thread SLEEPS → ~5,000-50,000 ns to wake
```

```
rte_eth_rx_burst() timeline:

  your code          no mode switch          NIC ring
  ──────────         ──────────────          ────────
  rx_burst() ──────► read tail pointer ───► get mbuf ptrs
             ◄────── return immediately
  you have 0-32 packets
  ~20-50 ns total
  if 0 packets: loop back immediately (no sleep)
```

**Where this is in YOUR code:**

In `socket_path.h` — the syscall cost is modeled by the condition_variable:
```cpp
void worker_loop() {
    std::unique_lock<std::mutex> lock(mtx_);
    cv_.wait(lock, [this] { return !queue_.empty() || !running_; });
    // ↑ THIS WAIT is the "recvfrom blocking" equivalent.
    // When no packet is in queue_, thread parks here.
    // Wakeup cost = ~5,000-50,000 ns (that's the real number on your Mac)
}
```

In `dpdk_path.h` — the burst cost is modeled by pop_burst:
```cpp
void lcore_poll_loop() {
    while (running_) {
        size_t n = rx_ring_.pop_burst(burst, BURST_SIZE);
        if (n == 0) continue;  // ← no sleep. just loop back. ~20ns.
        process_burst(burst, n);
    }
}
```

**The number to remember:**
Our benchmark: socket p50 = **327,958 ns** vs DPDK-style p50 = **167 ns**.
That ~327,000 ns is almost entirely the condition_variable wakeup time.
The actual packet processing (PDR/FAR lookup) is the same in both paths.

---

### Concept 3 — Hardware Interrupts vs Poll Mode Driver (PMD)

**How hardware interrupts work (normal NIC):**
```
1. Packet arrives at NIC
2. NIC asserts interrupt line on CPU
3. CPU stops whatever it's doing
4. Runs interrupt service routine (ISR) in kernel
5. ISR copies packet into kernel memory
6. Signals the process waiting on recvfrom
7. Process wakes up (~5-50 μs later)
8. Process calls recvfrom → copies packet into user buffer

Cost: ~5-50 μs of wake latency + 2 memory copies
```

**How PMD works (DPDK NIC):**
```
1. At startup: VFIO maps NIC PCI memory into userspace.
   NIC interrupts are DISABLED.
2. Your lcore runs a tight while(true) loop forever.
3. Every iteration: read NIC's RX tail pointer (1 memory read).
4. If tail moved → packets arrived → copy pointers, done.
5. If tail same → no packets → loop immediately.

Cost: ~20-50 ns to check (whether or not packets arrived)
Trade-off: 1 CPU core runs at 100% even when network is idle.
```

**Where this is in YOUR code:**

`dpdk_path.h` — the PMD loop:
```cpp
void lcore_poll_loop() {
    while (running_) {          // ← never exits, never sleeps
        size_t n = rx_ring_.pop_burst(burst, BURST_SIZE);
        // pop_burst() = reads ring tail pointer
        // = what rte_eth_rx_burst() does on the NIC's DMA ring
        if (n == 0) continue;   // ← no interrupt, no sleep, just loop
        process_burst(burst, n);
    }
}
```

`socket_path.h` — the interrupt-driven equivalent:
```cpp
// cv_.notify_one() in enqueue() = "hardware interrupt fires"
// cv_.wait() in worker_loop() = "process was sleeping, now wakes"
// The wakeup latency = interrupt → ISR → process wake = ~5-50 μs
cv_.notify_one();   // equivalent: NIC fires interrupt
// ...
cv_.wait(lock, ...); // equivalent: process wakes from interrupt
```

**The 100% CPU trade-off in our simulator:**
Our DPDK-style lcore loop burns 100% CPU. You can verify this:
```bash
# In one terminal, run the benchmark:
cd 5g-simulator/build
./upf_benchmark --duration 5 --ues 10

# In another terminal:
top   # watch one core hit 100%
```
This is not a bug — it's the correct DPDK behavior.

---

### The 30-Second Answer — Say This Out Loud, Practice It

> "Normal Linux path: packet arrives, NIC fires a hardware interrupt, kernel
> handles it, copies the packet through its network stack into a socket buffer,
> your app calls recvfrom — that's a syscall, another mode switch — gets the
> packet, calls sendto to send it out — another syscall. Each syscall is 500
> to 2000 nanoseconds. If no packet is ready, your thread sleeps — waking up
> costs another 5 to 50 microseconds.
>
> DPDK path: NIC is unbound from its kernel driver and mapped directly into
> userspace. Your application runs a poll loop — it calls rte_eth_rx_burst
> in a tight while-true loop, no sleep, no syscall. Packets go from wire to
> your application in one DMA copy. Zero syscalls. Zero interrupts. The cost
> is one CPU core running at 100% forever.
>
> In our simulator, socket_path.h models the first approach using mutex and
> condition_variable. dpdk_path.h models the second using a lock-free SPSC
> ring. The benchmark shows 327 microseconds versus 167 nanoseconds — about
> 1,800x difference. The processing logic — PDR lookup, FAR forwarding — is
> identical in both. All the time difference is in how you receive and send
> the packet."

That's it. That answer covers all 3 concepts, references real code and real
numbers, and takes 35 seconds to say.

---

## 9. What You Can and Cannot Simulate on Mac

> **Bottom line: you can learn and explain everything on Mac. You cannot run
> real DPDK on Mac. The simulator is honest about this — and that honesty
> is actually a strength in an interview.**

---

### What You CAN Do on Mac (with our simulator)

```
✅  Run socket_path vs dpdk_path benchmark
    → Real latency difference (~1,800x) is visible on Mac
    → condition_variable overhead is genuine macOS kernel overhead
    → lock-free SPSC ring latency is genuine (~167ns)

✅  Understand every concept
    → All 3 concepts above map to actual running code
    → You can open the files, point at lines, explain functions

✅  Demo in an interview
    → cd 5g-simulator/build && ./upf_benchmark --duration 3
    → Shows real numbers, real output, real comparison table

✅  Explain the architecture
    → VFIO/UIO binding → explained in Section 4 of this doc
    → Hugepages → explained, even though Mac doesn't use them
    → rte_eth_rx_burst → mapped to pop_burst() in dpdk_path.h
    → mbuf pool → mapped to MbufPool in mbuf_pool.h

✅  The lock-free ring code (ring_buffer.h) is production-quality
    → std::atomic with acquire/release — correct memory ordering
    → alignas(64) on head/tail — real cache-line padding
    → Same algorithm as rte_ring in real DPDK
```

---

### What You CANNOT Do on Mac (real DPDK needs Linux + physical NIC)

```
❌  Bind NIC to VFIO/UIO
    Why: macOS has no VFIO/UIO kernel module.
    macOS doesn't expose PCI device memory to userspace this way.

❌  Use hugepages (/dev/hugepages)
    Why: macOS has no hugetlbfs. macOS uses different VM page sizes.
    You can allocate large aligned memory (mmap with MAP_ANON) 
    but it's NOT the same as Linux hugepages backed by TLB-optimized
    physical pages.

❌  Run real rte_eth_rx_burst() against a NIC
    Why: PMD needs the NIC's PCI BAR (Base Address Register) mapped
    into userspace. Not possible on macOS without a custom driver.

❌  Test with real GTP-U packets
    Why: Would need a real gNB or traffic generator sending UDP/GTP-U.

❌  Run dpdk-devbind.py or rte_eal_init()
    Why: EAL (Environment Abstraction Layer) requires Linux hugepages,
    NUMA topology, and VFIO — none available on macOS.
```

---

### How to Say This in an Interview

**If asked "Can you run real DPDK on your Mac?"**

> "No — real DPDK requires Linux with VFIO support and hugepages configured,
> plus a NIC with a supported PMD (like mlx5 for Mellanox or i40e for Intel).
> On Mac, none of those are available.
>
> What I built is an architectural simulation: the lock-free SPSC ring models
> rte_eth_rx_burst, the MbufPool models rte_pktmbuf_pool_create, and the poll
> loop models the lcore. The latency numbers are real — condition_variable
> wakeup on macOS kernel costs the same as on Linux (~5-50 μs). The DPDK-style
> path shows ~167 ns because the ring check is genuinely that fast.
>
> On a real Linux server with a Mellanox NIC and hugepages, the DPDK path would
> be even faster because you'd also eliminate the memcpy we do when enqueuing
> into the ring — real DMA would write directly into the mbuf."

**If asked "Have you run DPDK on Linux?"**

> "Not against a real NIC. I've read the rte_eth_rx_burst PMD internals and the
> VFIO/hugepage setup, and I've modeled the architecture in C++. To run real DPDK
> I'd need a Linux VM with hugepages configured and a NIC bound to vfio-pci —
> that's a 30-minute setup on any cloud VM. The concepts and code are the same;
> the only thing missing is the actual NIC hardware."

---

### The Honest 1-Liner for Interviews

> "I built a C++ simulator that models DPDK architecture — lock-free ring,
> mbuf pool, burst polling — and benchmarked it against the kernel socket path.
> I haven't run real DPDK against physical hardware, but I understand every
> component and why it exists. On a Linux server with hugepages and VFIO, the
> production setup would take 30 minutes."

This is the right level of honesty. It shows you understand the architecture
deeply without overclaiming production experience you don't have.
