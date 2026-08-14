# DPDK UPF Fast-Path — Complete Walkthrough

> **Who this is for:** Someone who knows socket programming (connect, send, recv) but has
> never used DPDK. Every DPDK word is explained in plain English first.
> Read top to bottom once. The diagrams are the most important part.

---

## The One Diagram You Must Know

This is the full DPDK pipeline. Everything else in this document is explaining what
each box in this diagram actually does.

```
          WIRE  (physical cable, packets arriving)
            |
            v
        +-------+
        |  NIC  |   Network Interface Card — your ethernet/fiber card
        +-------+
            |
            | DMA  ← NIC copies packet into RAM BY ITSELF, no CPU involved
            v
   ===================
   RX RING  (HW/DPDK)    ← hardware descriptor ring, NIC fills this
   ===================
            |
            | rte_eth_rx_burst()  ← lcore reads this ring (no syscall, no interrupt)
            v
        RX lcore             ← one CPU core pinned here, does nothing else
            |
            | push to software ring
            v
   ===================
   rte_ring  (SW)           ← software queue between lcores (lock-free)
   ===================
            |
            | pop from ring
            v
      Worker lcore           ← does PDR/FAR/QER lookup, decides what to do
            |
            | push result to TX ring
            v
   ===================
   rte_ring  (SW)           ← another software queue, going toward TX
   ===================
            |
            | pop
            v
        TX lcore             ← one CPU core pinned here for sending
            |
            | rte_eth_tx_burst()  ← writes to NIC TX ring, NIC sends it
            v
   ===================
   TX RING  (HW/DPDK)       ← hardware descriptor ring, NIC reads this to send
   ===================
            |
            v
          WIRE  (packet goes out on the cable)
```

**In our code:** We simulate this with one lcore that does RX + processing together
(no separate TX lcore). The diagram above is the full production architecture.
Our simulation maps to: `rx_ring_` = RX RING, `lcore_poll_loop()` = RX lcore + Worker lcore combined.

---

## Why This Exists — The Problem DPDK Solves

### How normal Linux networking works (what you already know)

You know socket programming. Here is what happens in the kernel when a packet arrives:

```
Packet arrives at NIC
        |
        v
NIC raises a hardware INTERRUPT  ← stops whatever CPU was doing (~1-5 microseconds)
        |
        v
Linux kernel interrupt handler runs
        |
        v
Kernel allocates sk_buff (packet buffer)  ← kmalloc, can be slow
        |
        v
Kernel copies packet bytes into sk_buff   ← memcpy, uses L1/L2 cache
        |
        v
Kernel runs network stack (Ethernet → IP → UDP → GTP-U)
        |
        v
Your app calls recvfrom() — a SYSCALL     ← user/kernel mode switch ~500ns
        |
        v
Kernel copies data from sk_buff to your buffer  ← another memcpy
        |
        v
Your app finally sees the packet
        |
        v
Your app calls sendto() — another SYSCALL ← another user/kernel mode switch
        |
        v
Kernel copies data to send buffer         ← another memcpy
        |
        v
NIC sends it
```

**Total overhead: 5,000 - 50,000 nanoseconds per packet.**
At 1 million packets per second (1 Mpps): your CPU spends most of its time in kernel overhead.

---

### How DPDK works (the bypass)

```
Packet arrives at NIC
        |
        v
NIC uses DMA to write directly into a pre-allocated buffer (mbuf)
NO INTERRUPT. NO KERNEL INVOLVED.
        |
        v
Your C++ code calls rte_eth_rx_burst()   ← NOT a syscall, just reads a memory address
        |
        v
You process the packet in YOUR code
        |
        v
You call rte_eth_tx_burst()              ← NOT a syscall, just writes a memory address
        |
        v
NIC reads the TX ring and sends it       ← NIC polls the ring itself
```

**Total overhead: 50 - 500 nanoseconds per packet.**
**That is 10x to 100x faster.**

---

## The Three Things You Must Understand

### Thing 1: Hugepages

Normal RAM works in 4KB chunks called "pages". The CPU has a small cache called TLB
that remembers where pages are. When the TLB is full and you access a new page,
the CPU has to look up the address — this takes ~100 nanoseconds extra.

At 1 million packets per second with 1500-byte packets:
- Normal 4KB pages: each packet needs a new TLB lookup → 100ns × 1M = 100ms wasted per second
- 2MB hugepages: one TLB entry covers 512 packets → almost zero TLB misses

```
NORMAL PAGES (4KB):
┌──────┬──────┬──────┬──────┬──────┬──────┐  ← many small pages
│ 4KB  │ 4KB  │ 4KB  │ 4KB  │ 4KB  │ 4KB  │
└──────┴──────┴──────┴──────┴──────┴──────┘
  ↑                                    ↑
  One packet = ~one page                Each needs its own TLB entry
  TLB needs 36,864 entries for 65536 mbufs

HUGEPAGES (2MB):
┌────────────────────────┬────────────────────────┐  ← few big pages
│         2MB            │         2MB             │
│  (512 packets fit here)│  (512 packets fit here) │
└────────────────────────┴────────────────────────┘
  TLB needs only 72 entries for the same 65536 mbufs
```

**In our code:** `MbufPool` uses `std::vector` (normal heap). Real DPDK uses hugepages via
`rte_pktmbuf_pool_create()` which allocates from `/mnt/huge`. See `mbuf_pool.h`.

---

### Thing 2: Mbufs (Memory Buffers)

In normal Linux: each packet gets a fresh allocation (`kmalloc`). Slow.
In DPDK: you pre-allocate a pool of fixed-size buffers at startup. Each buffer is called an **mbuf**.

When a packet arrives, the NIC doesn't allocate — it just uses the next available mbuf
from the pre-allocated pool. When you're done with a packet, you return the mbuf to the pool.
No malloc, no free, no kernel involved.

```
MBUF POOL (allocated at startup from hugepages):
┌─────────────────────────────────────────────────────────────────┐
│  mbuf[0]    mbuf[1]    mbuf[2]   ...   mbuf[65535]              │
│ ┌────────┐ ┌────────┐ ┌────────┐      ┌────────┐               │
│ │ header │ │ header │ │ header │ ...  │ header │  ← metadata   │
│ │ 64bytes│ │ 64bytes│ │ 64bytes│      │ 64bytes│  (pkt_len,    │
│ ├────────┤ ├────────┤ ├────────┤      ├────────┤   port, etc)  │
│ │headrm  │ │headrm  │ │headrm  │      │headrm  │  ← 128 bytes  │
│ │128bytes│ │128bytes│ │128bytes│      │128bytes│  for prepending│
│ ├────────┤ ├────────┤ ├────────┤      ├────────┤  GTP headers  │
│ │ data   │ │ data   │ │ data   │      │ data   │  ← actual     │
│ │  1984  │ │  1984  │ │  1984  │      │  1984  │  packet bytes │
│ │ bytes  │ │ bytes  │ │ bytes  │      │ bytes  │               │
│ └────────┘ └────────┘ └────────┘      └────────┘               │
│  in_use=0   in_use=1   in_use=0              in_use=0           │
└─────────────────────────────────────────────────────────────────┘
          ↑             ↑
        NIC DMA       currently being processed
        writing here   by lcore
```

**The headroom is important:** When you need to add a GTP-U header to a downlink packet
(wrapping the inner IP packet in GTP-U to send to the gNB), you just write into the
headroom bytes BEFORE the packet — no memcpy of the whole packet needed.

**In our code:** `struct Mbuf` in `mbuf_pool.h` — it wraps our `Packet` struct.
Real DPDK equivalent: `rte_mbuf` (the actual struct used in production).

---

### Thing 3: Poll Mode Driver (PMD) and the Descriptor Ring

In normal Linux: NIC interrupts the CPU when a packet arrives.
In DPDK: the NIC has a ring of "descriptors" in RAM. Your lcore checks this ring in a tight loop.

```
NIC RX DESCRIPTOR RING (hardware, in RAM):
                            ← NIC is filling packets this way (head moves right)
┌────┬────┬────┬────┬────┬────┬────┬────┐
│ D0 │ D1 │ D2 │ D3 │ D4 │ D5 │ D6 │ D7 │   D = Descriptor
└────┴────┴────┴────┴────┴────┴────┴────┘
  ↑                   ↑
 tail               head
(lcore reads       (NIC writes
 from here)         here)

Each descriptor = pointer to one mbuf + done_flag

When NIC fills a packet:
  1. NIC DMA copies packet bytes into mbuf[i].data
  2. NIC sets descriptor[i].done = 1
  3. NIC moves head forward

When lcore calls rte_eth_rx_burst(port, queue, mbufs, 32):
  1. PMD reads from tail up to 32 descriptors where done=1
  2. Fills mbufs[] array with pointers to those mbufs
  3. Moves tail forward
  4. Returns count of packets found (0 if ring empty)
  5. ENTIRE THING IS ~50-200ns. No syscall. No interrupt.
```

**In our code:** `SPSCRing<Mbuf*, 4096>` in `ring_buffer.h` simulates this.
`pop_burst()` is our equivalent of `rte_eth_rx_burst()`.

---

## The NIC Queues Diagram — Hardware to Software

This is the full memory layout from NIC hardware to your C++ code:

```
┌─────────────────────────────────────────────────────────────────────┐
│  HARDWARE (NIC chip)                                                │
│                                                                     │
│   N3 physical port (from gNB)    N6 physical port (to internet)    │
│   ┌─────────────────┐            ┌─────────────────┐               │
│   │  RX queue 0     │            │  RX queue 0     │               │
│   │  [d][d][d][d]   │            │  [d][d][d][d]   │  d=descriptor │
│   └────────┬────────┘            └────────┬────────┘               │
│            │ DMA                           │ DMA                   │
└────────────┼───────────────────────────────┼─────────────────────┘
             │                               │
             ↓                               ↓
┌─────────────────────────────────────────────────────────────────────┐
│  HUGEPAGE MEMORY (2MB pages, pinned — never swapped out)            │
│                                                                     │
│  rte_mempool "n3_pool"                 rte_mempool "n6_pool"        │
│  ┌──────────────────────┐              ┌──────────────────────┐     │
│  │ mbuf[0] ..mbuf[65535]│              │ mbuf[0] ..mbuf[65535]│     │
│  │ (NIC DMA writes here)│              │ (NIC DMA writes here)│     │
│  └──────────────────────┘              └──────────────────────┘     │
│                                                                     │
│  Per-lcore cache (fast path):                                       │
│  lcore1_cache[0..511] → mbufs ready to alloc (no global ring touch)│
└─────────────────────────────────────────────────────────────────────┘
             │                               │
             ↓                               ↓
┌─────────────────────────────────────────────────────────────────────┐
│  USERSPACE C++ CODE (your DpdkPath class)                           │
│                                                                     │
│  lcore_poll_loop() runs on CPU core 1, pinned, 100% busy            │
│                                                                     │
│  while(true) {                                                      │
│    n = rte_eth_rx_burst(N3_PORT, 0, burst, 32);  // poll NIC       │
│    process_burst(burst, n);   // PDR/FAR/QER lookup                 │
│    rte_eth_tx_burst(N6_PORT, 0, tx_burst, tx_n); // send to N6     │
│  }                                                                  │
└─────────────────────────────────────────────────────────────────────┘
```

---

## UL Packet Journey: gNB → UPF → Internet (Uplink)

Step by step. Every function name here matches either our code or real DPDK.

```
gNB (base station) sends a GTP-U packet on UDP port 2152
        |
        | (on the wire, this is what the packet looks like:)
        | ┌──────────────────────────────────────────────────────┐
        | │ Outer Ethernet header  (14 bytes)                    │
        | │ Outer IP header        (20 bytes) — gNB IP → UPF IP  │
        | │ Outer UDP header       (8 bytes)  — port 2152        │
        | │ GTP-U header           (8 bytes)  — contains TEID    │
        | │   TEID = 1001  ← tunnel ID telling UPF "this is UE1" │
        | │ Inner IP header        (20 bytes) — UE IP → internet │
        | │ Inner TCP/UDP          (payload data)                │
        | └──────────────────────────────────────────────────────┘
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 1: NIC receives packet (N3 port)                  │
│                                                         │
│  NIC DMA engine writes bytes into mbuf.data             │
│  No CPU interrupt. No kernel. DMA runs independently.   │
│                                                         │
│  mbuf.pkt_len = 70 bytes (all headers + payload)        │
│  mbuf.data_off = 0  (start of ethernet header)          │
└─────────────────────────────────────────────────────────┘
        |
        | descriptor ring: done_flag set to 1
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 2: lcore_poll_loop() notices descriptor ready     │
│                                                         │
│  // Our simulation:                                     │
│  n = rx_ring_.pop_burst(burst, BURST_SIZE)              │
│                                                         │
│  // Real DPDK:                                          │
│  n = rte_eth_rx_burst(N3_PORT, 0, burst, 32)            │
│                                                         │
│  n = 32  (got 32 packets this round)                    │
│  No return, no sleep. Immediately goes to process_burst()│
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 3: process_burst(burst, 32) called ONCE           │
│                                                         │
│  uint64_t now = Packet::now_ns()   // ONE timestamp for │
│                                    // all 32 packets    │
│                                                         │
│  for (i = 0 to 31) {                                    │
│    Mbuf* m = burst[i];                                  │
│    Packet& pkt = m->pkt;                                │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 4: Parse TEID from raw packet bytes               │
│                                                         │
│  (Real DPDK only — our sim has TEID pre-filled)         │
│  const uint8_t* data = rte_pktmbuf_mtod(m, uint8_t*);  │
│                                                         │
│  // TEID is at byte offset 46 from start of frame:      │
│  // 14 (Eth) + 20 (IP) + 8 (UDP) + 4 (GTP-U flags)     │
│  uint32_t teid = ntohl(*(uint32_t*)(data + 46));        │
│                                                         │
│  teid = 1001  ← this identifies UE #1                   │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 5: PDR lookup (the "which UE is this?" step)      │
│                                                         │
│  const PDR* pdr = rules_.match_pdr(pkt)                 │
│    → teid_to_pdr_.find(1001)   // hash map lookup O(1)  │
│    → returns pdr_id = 1                                 │
│    → PDR #1: {teid=1001, dir=UL, far_id=1, qer_id=1}   │
│                                                         │
│  pkt.pdr_id = 1;                                        │
│  pkt.far_id = 1;                                        │
│  pkt.classified = true;                                 │
│                                                         │
│  // Real DPDK: rte_hash_lookup_data(teid_table, &teid)  │
│  // ~80ns including hash computation (vs ~150ns std::map)│
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 6: QER lookup (the "how fast is this UE?" step)   │
│                                                         │
│  const QER* qer = rules_.get_qer(pdr->qer_id)           │
│    → qers_.find(1)                                      │
│    → QER #1: {qfi=1, ul_mbr=64kbps, dl_mbr=64kbps}     │
│                (QFI=1 means voice call, 64kbps each way) │
│                                                         │
│  // In production: token bucket check here              │
│  // if (token_bucket_check(qer, pkt.payload_len) == DROP)│
│  //   drop packet and free mbuf                         │
│  // (not implemented in our simulation)                 │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 7: FAR lookup (the "what do I do?" step)          │
│                                                         │
│  const FAR* far = rules_.get_far(pdr->far_id)           │
│    → fars_.find(1)                                      │
│    → FAR #1: {action=FORWARD, dst_ip=8.8.8.8,           │
│               encap_gtpu=false}                         │
│                                                         │
│  action = FORWARD, encap_gtpu = false                   │
│  → This is UL: strip GTP-U, send inner IP to internet   │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 8: Strip outer GTP-U header (UL path)             │
│                                                         │
│  // Real DPDK:                                          │
│  rte_pktmbuf_adj(m, 46 + 8)  // advance past GTP-U     │
│  // Now mbuf points to inner IP packet only             │
│                                                         │
│  // Inner packet:                                       │
│  // ┌──────────────────────────────┐                    │
│  // │ Inner IP: UE_IP → 8.8.8.8   │                    │
│  // │ Inner TCP/UDP: actual data   │                    │
│  // └──────────────────────────────┘                    │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 9: Send to N6 (internet side)                     │
│                                                         │
│  // Real DPDK:                                          │
│  tx_burst[tx_n++] = m;                                  │
│  // (after burst is full or end of rx burst:)           │
│  rte_eth_tx_burst(N6_PORT, 0, tx_burst, tx_n);          │
│                                                         │
│  // NIC TX ring gets the descriptor                     │
│  // NIC DMA reads from mbuf.data and puts on the wire   │
│  // After TX done: rte_pktmbuf_free(m) returns mbuf     │
│  //               to per-lcore cache for reuse          │
│                                                         │
│  stats_.pkts_forwarded.fetch_add(1)   // count it       │
│  stats_.record_latency(arrival_ns, now)                 │
│  mbuf_pool_.free_mbuf(m)              // our simulation  │
└─────────────────────────────────────────────────────────┘
        |
        v
Packet is on the internet heading to 8.8.8.8
```

---

## DL Packet Journey: Internet → UPF → gNB (Downlink)

```
Server on internet (e.g., 8.8.8.8) sends reply to UE IP 10.45.0.1
        |
        | (on the wire, plain IP packet, no GTP-U yet:)
        | ┌──────────────────────────────────┐
        | │ Ethernet header                  │
        | │ IP header: 8.8.8.8 → 10.45.0.1  │ ← UE's IP
        | │ TCP/UDP payload                  │
        | └──────────────────────────────────┘
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 1: NIC receives packet (N6 port, from internet)   │
│  NIC DMA writes into mbuf. No interrupt.                │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 2: lcore_poll_loop() picks it up                  │
│  n = rte_eth_rx_burst(N6_PORT, 0, burst, 32)            │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 3: process_burst() — single timestamp for batch   │
│  uint64_t now = rte_rdtsc() / tsc_hz * 1e9              │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 4: Parse destination IP from inner IP header      │
│                                                         │
│  const uint8_t* data = rte_pktmbuf_mtod(m, uint8_t*);  │
│  // IP dest is at offset 30: 14 (Eth) + 16 (dst in IP) │
│  uint32_t ue_ip = ntohl(*(uint32_t*)(data + 30));       │
│                                                         │
│  ue_ip = 10.45.0.1   ← the UE's IP address              │
│  pkt.dir = Direction::DL;                               │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 5: PDR lookup (DL uses UE IP as key, not TEID)    │
│                                                         │
│  const PDR* pdr = rules_.match_pdr(pkt)                 │
│    → ueip_to_pdr_.find(10.45.0.1)  // hash map O(1)    │
│    → returns pdr_id = 2                                 │
│    → PDR #2: {ue_ip=10.45.0.1, dir=DL, far_id=2}       │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 6: FAR lookup — DL FAR says encap_gtpu=true       │
│                                                         │
│  const FAR* far = rules_.get_far(2)                     │
│  → FAR #2: {action=FORWARD, dst_ip=10.10.0.1,           │
│             dst_port=2152, encap_gtpu=true}              │
│                                                         │
│  encap_gtpu=true → we must WRAP the packet in GTP-U     │
│  dst_ip=10.10.0.1 → gNB's N3 IP address                │
│  dst_port=2152    → standard GTP-U port                 │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 7: Prepend GTP-U outer header (DL path)           │
│                                                         │
│  // Real DPDK — this is the headroom in action:         │
│  char* hdr = rte_pktmbuf_prepend(m, sizeof(gtp_outer)); │
│  // This just moves data_off backward into headroom.    │
│  // NO memcpy of the existing packet bytes.             │
│                                                         │
│  fill_gtp_header(hdr, teid=1001, inner_pkt_len);        │
│  fill_udp_header(..., dst_port=2152);                   │
│  fill_ip_header(..., dst_ip=10.10.0.1);                 │
│  fill_eth_header(...);                                  │
│                                                         │
│  // Packet is now:                                      │
│  // ┌──────────────────────────────────────────────┐    │
│  // │ Outer Eth: UPF_MAC → gNB_MAC                │    │
│  // │ Outer IP:  UPF_N3_IP → gNB_N3_IP            │    │
│  // │ Outer UDP: sport=random → dport=2152         │    │
│  // │ GTP-U:     TEID=1001                         │    │
│  // │ Inner IP:  8.8.8.8 → 10.45.0.1              │    │
│  // │ TCP payload                                  │    │
│  // └──────────────────────────────────────────────┘    │
└─────────────────────────────────────────────────────────┘
        |
        v
┌─────────────────────────────────────────────────────────┐
│  STEP 8: Send to N3 (gNB side)                          │
│                                                         │
│  rte_eth_tx_burst(N3_PORT, 0, &m, 1)                    │
│  NIC reads TX descriptor, DMA sends packet on the wire  │
│  gNB receives it, strips GTP-U, delivers to UE          │
└─────────────────────────────────────────────────────────┘
        |
        v
Packet reaches the UE (10.45.0.1) via gNB radio link
```

---

## The SPSC Ring — How Data Moves Between lcore and Worker

Our `ring_buffer.h` (the `SPSCRing` class) simulates the `rte_ring`.
Here is the ring state at each moment:

```
INITIAL STATE (empty ring):
  head = 0, tail = 0, N = 8 (example, actually 4096 in code)
  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │    │    │    │    │    │    │    │    │
  └────┴────┴────┴────┴────┴────┴────┴────┘
    h,t                                      h = head (producer writes here)
                                             t = tail (consumer reads from here)

AFTER push(mbuf0), push(mbuf1), push(mbuf2):
  head = 3, tail = 0
  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │mb0 │mb1 │mb2 │    │    │    │    │    │
  └────┴────┴────┴────┴────┴────┴────┴────┘
    t              h

AFTER pop_burst(out, 3):
  head = 3, tail = 3   (ring empty again)
  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │mb0 │mb1 │mb2 │    │    │    │    │    │
  └────┴────┴────┴────┴────┴────┴────┴────┘
                  h,t
  out[] = {mb0, mb1, mb2}, returns 3

WRAP-AROUND (head near end, then wraps to 0):
  head = 7, tail = 5
  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │    │    │    │    │    │mb5 │mb6 │    │
  └────┴────┴────┴────┴────┴────┴────┴────┘
                              t         h
  After push(mb7): head = (7+1) & 7 = 0 → wraps!
  ┌────┬────┬────┬────┬────┬────┬────┬────┐
  │    │    │    │    │    │mb5 │mb6 │mb7 │
  └────┴────┴────┴────┴────┴────┴────┴────┘
    h                       t

WHY N MUST BE POWER OF 2:
  next = (head + 1) % N   ← normal modulo, expensive division
  next = (head + 1) & (N-1) ← bitmask trick, 1 CPU instruction, only works for power-of-2 N
```

**False sharing** — why `alignas(64)` on head_ and tail_:
```
WITHOUT alignas(64):
  [head_][tail_][buf...]    ← head and tail on same 64-byte cache line
  
  Producer updates head_: marks cache line MODIFIED on CPU0
  Consumer reads tail_:   CPU1 must fetch that cache line from CPU0 (~100-200ns)
  Consumer updates tail_: marks cache line MODIFIED on CPU1
  Producer reads head_:   CPU0 must fetch from CPU1 (~100-200ns)
  
  Every push/pop causes a cache line bounce between CPU cores. Slow.

WITH alignas(64):
  [head_][padding...][tail_][padding...][buf...]
  head_ is on its own 64-byte cache line → only CPU0 touches it
  tail_ is on its own 64-byte cache line → only CPU1 touches it
  No cache line bouncing. Each core operates independently.
```

---

## Memory Pool and Per-lcore Cache

This is how `rte_pktmbuf_alloc()` works in real DPDK (and what we simulate with `MbufPool::alloc()`):

```
rte_mempool structure:
┌────────────────────────────────────────────────────────────────────┐
│  GLOBAL RING (rte_ring, lock-free MPMC)                            │
│  Contains pointers to ALL free mbufs                               │
│  ┌────┬────┬────┬────┬────┬────┬────┬────┬────┐                   │
│  │m0  │m1  │m2  │m3  │m4  │m5  │m6  │m7  │... │                   │
│  └────┴────┴────┴────┴────┴────┴────┴────┴────┘                   │
│                           ↑                                        │
│                     pulled in batches of 512 when                  │
│                     per-lcore cache runs out                       │
├────────────────────────────────────────────────────────────────────┤
│  PER-LCORE CACHE (one per CPU core, not shared)                    │
│                                                                    │
│  lcore0 cache: [m10][m11][m12]...[m521]  (512 entries max)        │
│  lcore1 cache: [m522][m523]...[m1033]    (512 entries max)        │
│  lcore2 cache: [m1034]...               (512 entries max)         │
│                                                                    │
│  rte_pktmbuf_alloc(pool):                                          │
│    if lcore_cache not empty:                                        │
│      return lcore_cache.pop()  ← ~5 CPU cycles, no global ring     │
│    else:                                                           │
│      refill lcore_cache from global ring (~30 cycles)              │
│      return lcore_cache.pop()                                      │
│                                                                    │
│  rte_pktmbuf_free(mbuf):                                           │
│    if lcore_cache not full:                                         │
│      lcore_cache.push(mbuf)  ← ~5 CPU cycles, no global ring       │
│    else:                                                           │
│      drain 256 entries from lcore_cache to global ring (~30 cycles)│
│      lcore_cache.push(mbuf)                                        │
└────────────────────────────────────────────────────────────────────┘

OUR SIMULATION (mbuf_pool.h):
  pool_ = std::vector<Mbuf> (65536 elements, heap memory)
  next_ = atomic counter, just increments and wraps
  alloc() = &pool_[next_++]   ← same concept, simpler implementation
  free_mbuf(m) = m->in_use = false  ← not actually used for realloc in sim
```

---

## Socket Path vs DPDK Path — Side by Side

```
SAME PACKET, TWO PATHS:

SOCKET PATH (socket_path.h)          │  DPDK PATH (dpdk_path.h)
                                      │
Packet arrives at NIC                 │  Packet arrives at NIC
        ↓                             │          ↓
NIC raises interrupt                  │  NIC DMA fills mbuf (no interrupt)
~1-5μs to handle interrupt            │  ~0ns — NIC works independently
        ↓                             │          ↓
Kernel IRQ handler runs               │  lcore busy-polls rx_ring_
Kernel allocates sk_buff (~100-500ns) │  pop_burst() reads ring (~10ns)
        ↓                             │          ↓
Kernel copies data to sk_buff         │  mbuf already has data (DMA done)
~10-30ns + cache pollution            │  ~0ns — no copy
        ↓                             │          ↓
recvfrom() syscall                    │  No syscall
~500-2000ns (user↔kernel switch)      │  ~0ns
        ↓                             │          ↓
Kernel copies to userspace buffer     │  Already in userspace mbuf
~10-30ns                              │  ~0ns
        ↓                             │          ↓
enqueue() — mutex lock                │  rx_ring_.push() — lock-free
~20-100ns uncontended                 │  ~5-10ns (atomic store)
        ↓                             │          ↓
cv.notify_one() — futex syscall       │  No notification needed
~200-1000ns                           │  ~0ns
        ↓                             │          ↓
Worker thread wakes up                │  Already processing
~5000-50000ns (OS scheduler)          │  ~0ns — same tight loop
        ↓                             │          ↓
PDR/QER/FAR lookup (~150ns/pkt)       │  PDR/QER/FAR lookup (~80ns/pkt)
        ↓                             │          ↓
sendto() syscall                      │  rte_eth_tx_burst() — no syscall
~500-2000ns                           │  ~50-200ns for full burst of 32
─────────────────────────────────────────────────────────────────────
TOTAL per packet: ~6000-60000ns       │  ~100-500ns
                = 6-60 MICROSECONDS   │  = 0.1-0.5 MICROSECONDS
                                      │
Max throughput: ~200K-1Mpps           │  Max throughput: ~10-40Mpps
```

---

## Our Code Files — What Each One Does

```
src/upf_fastpath/
├── packet.h          ─ Packet struct: TEID, UE_IP, DSCP, QFI, timestamps
│                       "The data that describes one network packet"
│
├── pfcp_rules.h      ─ PDR, FAR, QER structs + RuleTable with hash maps
│                       "The forwarding rules the SMF programs into the UPF"
│                       build_demo_rules() creates fake rules for N UEs
│
├── ring_buffer.h     ─ SPSCRing<T, N> lock-free ring
│                       "Simulates rte_ring and the NIC descriptor ring"
│
├── mbuf_pool.h       ─ MbufPool and Mbuf struct
│                       "Simulates rte_mempool + rte_mbuf (hugepage pool)"
│
├── socket_path.h     ─ SocketPath class (mutex + condvar + queue)
│                       "Shows how a kernel socket-based UPF works (slow)"
│
├── dpdk_path.h       ─ DpdkPath class (SPSCRing + lcore poll loop)
│                       "Shows how a DPDK UPF works (fast)"
│
├── stats.h           ─ PathStats with reservoir sampling for percentiles
│                       "Counts packets, measures latency, computes p50/p99"
│
├── dpdk_compat.h     ─ Real DPDK calls (#ifdef ENABLE_DPDK)
│                       "Maps our simulation → real rte_* functions for Oracle Cloud"
│
├── upf_fastpath_demo.cpp  ─ Interactive demo: shows each packet step by step
│                            Run: ./upf_demo [--socket] [--dpdk] [--packets 20]
│
└── benchmark.cpp     ─ Throughput benchmark: runs millions of packets, prints table
                        Run: ./upf_benchmark [--ues 1000] [--seconds 5]
```

---

## Our Simulation vs Real DPDK — Function Mapping

| What we do (simulation) | Real DPDK call | What it does |
|---|---|---|
| `MbufPool(65536)` | `rte_pktmbuf_pool_create(name, 65536, 512, 0, 2176, socket_id)` | Create hugepage-backed mbuf pool |
| `mbuf_pool_.alloc()` | `rte_pktmbuf_alloc(pool)` | Get a free mbuf (~5 cycles) |
| `mbuf_pool_.free_mbuf(m)` | `rte_pktmbuf_free(m)` | Return mbuf to pool (~5 cycles) |
| `SPSCRing::push(mbuf*)` | not needed — NIC DMA fills mbufs directly | |
| `rx_ring_.pop_burst(n)` | `rte_eth_rx_burst(port, queue, mbufs, n)` | Poll NIC for up to n packets |
| `DpdkPath::start()` → thread | `rte_eal_remote_launch(fn, arg, lcore_id)` | Pin lcore function to CPU core |
| `Packet::now_ns()` | `rte_rdtsc() / rte_get_tsc_hz() * 1e9` | Read CPU clock (1 instruction) |
| `rules_.match_pdr()` with `unordered_map` | `rte_hash_lookup_data(ht, &teid)` | O(1) cuckoo hash lookup |
| `std::thread` | `rte_lcore` | Thread pinned to one CPU core |
| No equivalent | `rte_eth_tx_burst(port, 0, tx_burst, n)` | Send burst out NIC TX ring |
| `rte_eal_init()` not in sim | `rte_eal_init(argc, argv)` | Init DPDK runtime, hugepages, lcores |

---

## Simple Demo (10 lines to understand the concept)

This is the minimum to understand what DPDK does. No rules, no PDR/FAR, just the poll loop:

```cpp
// THE SIMPLEST POSSIBLE DPDK PROGRAM (conceptually)
// Real DPDK, stripped to bare minimum:

rte_eal_init(argc, argv);   // Step 1: init hugepages and lcores

rte_mempool* pool = rte_pktmbuf_pool_create(
    "my_pool", 65536, 512, 0, 2176, 0);  // Step 2: make mbuf pool

rte_eth_dev_configure(port=0, nb_rx_q=1, nb_tx_q=1, &conf);
rte_eth_rx_queue_setup(port=0, queue=0, 1024, 0, NULL, pool);
rte_eth_dev_start(port=0);              // Step 3: start the NIC

// Step 4: THE POLL LOOP — this runs forever on one CPU core
while (true) {
    rte_mbuf* burst[32];
    uint16_t n = rte_eth_rx_burst(0, 0, burst, 32);  // poll NIC
    for (int i = 0; i < n; i++) {
        // do something with burst[i] — lookup, forward, etc.
        rte_pktmbuf_free(burst[i]);  // return mbuf to pool
    }
}
// That's it. No sleep. No mutex. No syscall. Just: poll → process → free → repeat.
```

**In our code:** `DpdkPath::lcore_poll_loop()` in `dpdk_path.h` does exactly this.
`rx_ring_.pop_burst()` = `rte_eth_rx_burst()`.
`mbuf_pool_.free_mbuf(m)` = `rte_pktmbuf_free(m)`.

---

## Production Demo (what it looks like with the full pipeline)

```cpp
// PRODUCTION UPF — multi-lcore pipeline
// (matches the main diagram at the top of this doc)

// LCORE 0 (main): initialization only
rte_eal_init(argc, argv);
rte_mempool* pool = rte_pktmbuf_pool_create("n3_pool", 65536, 512, 0, 2176, 0);
configure_nic(N3_PORT, pool);  // N3 = gNB side
configure_nic(N6_PORT, pool);  // N6 = internet side

rte_ring* rx_to_worker = rte_ring_create("rx_work", 4096, 0, RING_F_SP_ENQ|RING_F_SC_DEQ);
rte_ring* worker_to_tx = rte_ring_create("work_tx", 4096, 0, RING_F_SP_ENQ|RING_F_SC_DEQ);

rte_eal_remote_launch(rx_lcore_fn, rx_args, lcore_id=1);     // pin RX to core 1
rte_eal_remote_launch(worker_lcore_fn, work_args, lcore_id=2); // pin Worker to core 2
rte_eal_remote_launch(tx_lcore_fn, tx_args, lcore_id=3);     // pin TX to core 3
rte_eal_mp_wait_lcore();  // wait for all

// LCORE 1 (RX): just pulls packets off NIC, hands to worker
int rx_lcore_fn(void* arg) {
    while (running) {
        rte_mbuf* burst[32];
        uint16_t n = rte_eth_rx_burst(N3_PORT, 0, burst, 32);
        if (n > 0) rte_ring_enqueue_burst(rx_to_worker, (void**)burst, n, NULL);
    }
}

// LCORE 2 (Worker): PDR/FAR/QER lookup, decides UL vs DL
int worker_lcore_fn(void* arg) {
    while (running) {
        rte_mbuf* burst[32];
        uint16_t n = rte_ring_dequeue_burst(rx_to_worker, (void**)burst, 32, NULL);
        for (int i = 0; i < n; i++) {
            process_packet(burst[i], rules);  // PDR + FAR + QER
        }
        if (n > 0) rte_ring_enqueue_burst(worker_to_tx, (void**)burst, n, NULL);
    }
}

// LCORE 3 (TX): pulls from worker ring, sends out NIC
int tx_lcore_fn(void* arg) {
    while (running) {
        rte_mbuf* burst[32];
        uint16_t n = rte_ring_dequeue_burst(worker_to_tx, (void**)burst, 32, NULL);
        if (n > 0) rte_eth_tx_burst(N6_PORT, 0, burst, n);
    }
}
```

---

## How to Run Our Code

```bash
# Go to the upf_fastpath directory
cd /Users/abhichauhan/Desktop/cpp-interview-prep/4g-5g-ims-simulators/5g-simulator/src/upf_fastpath

# Build
make

# Run the step-by-step demo (shows each packet with PDR/FAR/QER results)
./upf_demo --socket     # shows socket path overhead
./upf_demo --dpdk       # shows DPDK poll-mode path
./upf_demo              # runs both and compares

# Run the throughput benchmark (millions of packets)
./upf_benchmark --ues 100 --seconds 5
```

---

## Interview: What to Say

**Q: What is DPDK?**

> "DPDK is a set of libraries that lets your C++ application talk directly to the NIC,
> bypassing the Linux kernel entirely. Instead of the kernel handling interrupts and
> copying packets, your code polls the NIC in a tight loop. The NIC DMA-copies packets
> directly into pre-allocated buffers called mbufs that live in hugepage memory.
> Result: 10-100x faster packet processing — from ~50 microseconds per packet to
> ~500 nanoseconds."

**Q: What is a PMD?**

> "Poll Mode Driver — instead of interrupt-driven NIC access, the lcore continuously
> calls rte_eth_rx_burst() which reads the NIC's RX descriptor ring. If there are packets,
> it returns them immediately. If not, it returns 0 and you immediately loop back and poll again.
> One CPU core burns 100% doing this — but you get sub-microsecond packet latency."

**Q: What are hugepages and why do you need them?**

> "Hugepages are 2MB memory pages instead of the default 4KB. The CPU TLB (a small
> cache for address translations) can only hold ~1024 entries. At 4KB pages, a 1Mpps
> stream constantly evicts TLB entries, causing 100ns extra per-packet lookup overhead.
> With 2MB hugepages, one TLB entry covers 512 packets. DPDK requires hugepages for
> its mempool so the per-lcore cache fits entirely in TLB."

**Q: What is an mbuf?**

> "An mbuf is DPDK's packet buffer — like sk_buff in the Linux kernel but simpler.
> It has a metadata header (packet length, port, offload flags) followed by headroom
> (empty space before the data, used for prepending headers without memcpy) and the
> actual packet data. mbufs are pre-allocated at startup from hugepages and reused —
> alloc/free is ~5 CPU cycles vs ~100-500ns for kmalloc."

**Q: What is the difference between our simulation and real DPDK?**

> "Our simulation uses SPSCRing instead of rte_ring, MbufPool with std::vector instead
> of rte_mempool on hugepages, and std::thread instead of a pinned rte_lcore. The logic
> is identical: poll loop, burst processing, one timestamp per burst, lock-free rings.
> dpdk_compat.h has the real rte_* calls for when ENABLE_DPDK=1 on Oracle Cloud."
