# UPF Fast-Path Architecture Reference

Architecture reference for the DPDK-oriented UPF fast-path learning lab.

---

## 1. N3 / N4 / N6 Interface Overview

```
                              ┌──────────────────────────────────────────────┐
    RAN Side                  │                5G CORE                        │   Internet
                              │                                               │
 ┌─────────┐    N3            │  ┌──────────┐    N4 (PFCP)   ┌──────────┐   │    N6
 │   gNB   │◄── GTP-U/UDP ──►│  │   UPF    │◄──────────────►│   SMF    │   │  ┌──────┐
 │(base    │    port 2152     │  │          │                 │(Session  │   │  │ PDN  │
 │ station)│                  │  │ fast path│                 │ Mgmt Fn) │   │  │ GW   │
 └─────────┘                  │  │ DPDK/PMD │                 └────┬─────┘   │  └──────┘
                              │  │ PDR/FAR  │◄── plain IP ────────►│         │
                              │  │ QER      │    N6               N10        │
                              │  └──────────┘                      │         │
                              │                               ┌────▼─────┐   │
                              │                               │   UDM    │   │
                              │                               │(subscriber│   │
                              │                               │ database) │   │
                              │                               └──────────┘   │
                              └──────────────────────────────────────────────┘

N3: gNB ↔ UPF  — GTP-U encapsulated user plane packets (UDP port 2152)
N4: SMF ↔ UPF  — PFCP control messages (UDP port 8805); programs PDR/FAR/QER
N6: UPF ↔ Internet — plain IP packets (after GTP-U strip on UL / before add on DL)
```

### Interface Roles

**N3 (gNB → UPF, uplink):**
- Protocol: GTP-U (GPRS Tunneling Protocol — User Plane, 3GPP TS 29.281)
- Port: UDP 2152
- Outer IP: gNB N3 IP → UPF N3 IP
- Inner IP: UE's source IP (e.g., 10.45.0.1)
- GTP-U header: contains TEID (Tunnel Endpoint Identifier) — per-UE session identifier

**N4 (SMF → UPF):**
- Protocol: PFCP (Packet Forwarding Control Protocol, 3GPP TS 29.244)
- Port: UDP 8805
- Used to: create/modify/delete PFCP sessions, each session = one UE PDU session
- Carries: PDR, FAR, QER, URR (Usage Reporting Rules)
- Control-plane only: uses normal Linux socket (no DPDK)

**N6 (UPF → Internet):**
- Protocol: plain IPv4/IPv6
- No encapsulation
- UPF acts as the IP gateway for UEs
- Returns traffic arrives as plain IP destined to UE's IP address

---

## 2. PFCP Session Lifecycle: SMF → UPF Rule Programming

```
UE                   gNB            AMF            SMF             UDM             UPF
 │                    │              │              │               │               │
 │── PDU Session ────►│              │              │               │               │
 │   Setup Request    │── N2 Setup──►│              │               │               │
 │                    │              │── N11 ──────►│               │               │
 │                    │              │   PDU Sess   │── N10 ───────►│               │
 │                    │              │   SM Context │   Get Sub     │               │
 │                    │              │              │◄── Profile ───│               │
 │                    │              │              │               │               │
 │                    │              │              │─── N4 PFCP Session Est ──────►│
 │                    │              │              │    PDR(UL): TEID=1001         │
 │                    │              │              │    PDR(DL): UE-IP=10.45.0.1   │
 │                    │              │              │    FAR(UL): FORWARD to N6     │
 │                    │              │              │    FAR(DL): FORWARD+encap N3  │
 │                    │              │              │    QER: QFI=1, MBR=64kbps     │
 │                    │              │              │◄── PFCP Sess Est Response ────│
 │                    │              │              │    (F-TEID allocated)         │
 │                    │◄─ N2 Resp ──│◄─ N11 ──────│               │               │
 │◄── PDU Sess ───────│             │              │               │               │
 │    Accept          │             │              │               │               │
 │                    │             │              │               │               │
 │  [Data flows]      │             │              │               │               │
 │═══ GTP-U ─────────►│─────── N3 GTP-U ──────────────────────────────────────────►│
 │   TEID=1001        │             │              │               │  PDR match    │
 │                    │             │              │               │  FAR forward  │
 │                    │             │              │               │──────── N6 ──►│
```

Key PFCP message types:
- **Session Establishment Request**: creates a new PFCP session for a UE PDU session
- **Session Modification Request**: updates rules (e.g., handover TEID change, QoS update)
- **Session Deletion Request**: removes session on PDU session release
- **Session Report Request**: UPF → SMF with usage reports (URR data)

---

## 3. Packet Classification Pipeline: PDR → FAR → QER

Every packet arriving at the UPF goes through this pipeline:

```
Packet arrives (N3 GTP-U or N6 plain IP)
          │
          ▼
    ┌──────────────────────────────────┐
    │         PDR LOOKUP               │
    │  UL: hash(TEID) → PDR           │
    │  DL: hash(UE-IP) → PDR          │
    │  O(1) hash table lookup          │
    └──────────────┬───────────────────┘
                   │
          ┌────────┴────────┐
          │  PDR matched?   │
          └────────┬────────┘
         YES       │           NO
          │        │           │
          │        │      pkts_no_pdr++
          │        │      DROP
          ▼
    ┌──────────────────────────────────┐
    │         QER LOOKUP + CHECK       │
    │  get_qer(pdr.qer_id)            │
    │  Token bucket: within MBR?      │
    │  If over MBR: DROP/MARK         │
    └──────────────┬───────────────────┘
                   │
                   ▼
    ┌──────────────────────────────────┐
    │         FAR LOOKUP + ACTION      │
    │  get_far(pdr.far_id)            │
    │  FORWARD: route to dst          │
    │  DROP:    discard               │
    │  BUFFER:  hold for paging       │
    └──────────────┬───────────────────┘
                   │
                   ▼
    ┌──────────────────────────────────┐
    │         ENCAPSULATION (DL only)  │
    │  If FAR.encap_gtpu = true:      │
    │    add outer IP + UDP + GTP-U   │
    │    TEID from FAR.teid           │
    │    dst = gNB N3 IP:2152         │
    └──────────────┬───────────────────┘
                   │
                   ▼
              TX on N3 or N6
```

---

## 4. Socket Path — Detailed Per-Packet Flow and Overhead Analysis

### Code Path (socket_path.h)

```
Producer thread (simulates recvfrom() loop):
  enqueue(pkt):
    1. memcpy(&copy, &pkt, sizeof(Packet))          [OVERHEAD A: ~10-30ns + cache]
    2. std::lock_guard<std::mutex> lk(mtx_)         [OVERHEAD B: ~20-100ns uncontended]
    3. if (queue_.size() >= MAX_QUEUE_DEPTH) DROP   [back-pressure]
    4. queue_.push(copy)                             [O(1) std::deque push_back]
    5. cv_.notify_one()                              [OVERHEAD C: futex(FUTEX_WAKE)]

Consumer thread (simulates application worker):
  worker_loop():
    6. cv_.wait(lk, predicate)                      [OVERHEAD D: futex(FUTEX_WAIT)]
       - sleeps until notify_one() fires
       - wakeup latency: 5-50μs (Linux CFS scheduler)
    7. pkt = queue_.front(); queue_.pop()
    8. process_one(pkt):
         match_pdr(pkt)    → O(1) hash
         get_qer(qer_id)   → O(1) hash
         get_far(far_id)   → O(1) hash
         pkts_forwarded++
         record_latency()
    9. [sendto() in real UPF]                        [OVERHEAD E: ~500-2000ns syscall]
```

### Overhead Quantification

| Source | Cost | Explanation |
|---|---|---|
| memcpy (1400B packet) | ~10-30ns | + pollutes ~22 L1 cache lines |
| mutex lock (uncontended) | ~20-100ns | CAS on lock word |
| mutex lock (contended) | ~1-100μs | futex(FUTEX_WAIT) syscall + sleep |
| cv.notify_one() | ~200ns-1μs | futex(FUTEX_WAKE) syscall |
| Scheduler wakeup | ~5-50μs | Linux CFS: wakeup latency |
| recvfrom() [real] | ~500-2000ns | user↔kernel mode switch |
| sendto() [real] | ~500-2000ns | another user↔kernel switch |
| **Total per packet** | **~10μs - 100μs** | Dominated by cv wake |

---

## 5. DPDK Path — Poll Loop, Burst Processing, Mbuf Lifecycle

### Code Path (dpdk_path.h)

```
Producer thread (simulates NIC DMA + PMD):
  rx_enqueue(pkt):
    1. mbuf_pool_.alloc()        [~5ns: atomic bump-pointer]
    2. m->pkt = pkt              [~10-30ns: copy into mbuf]
    3. rx_ring_.push(m)          [~5-10ns: lock-free ring push]
       release-ordered store on head

lcore thread (simulates DPDK lcore_fn):
  lcore_poll_loop():
    while(running_):
      4. n = rx_ring_.pop_burst(burst, 32)  [~5-20ns if non-zero]
         acquire-ordered load on head
      if (n == 0): continue  [spin: ~1-2ns, no sleep]
      
      5. process_burst(burst, n):
         a. now = Packet::now_ns()           [ONE call for 32 packets: ~20-40ns]
         b. for each mbuf:
            match_pdr()     [O(1) hash, cache-warm after first hit: ~50-200ns]
            get_qer()       [O(1) hash]
            get_far()       [O(1) hash]
            pkts_forwarded++
            record_latency(pkt.arrival_ns, now)
            mbuf_pool_.free_mbuf(m)         [~2ns: set in_use=false]
```

### Mbuf Lifecycle

```
Startup:  MbufPool(65536)
          │
          │ pre-allocate 65536 Mbuf objects
          │ (in real DPDK: rte_pktmbuf_pool_create with hugepages)
          ▼
         [pool: 65536 mbufs, all free]

Per packet:
  alloc()   → mbuf* [atomic bump-pointer, ~5ns]
     │
  NIC DMA fills mbuf data (simulated by memcpy)
     │
  push to rx_ring_ [lock-free, ~5ns]
     │
  lcore pops from rx_ring_ in burst
     │
  process: PDR/FAR/QER lookup
     │
  free_mbuf() → set in_use=false [~2ns]
     │
  [mbuf available for re-alloc — bump-pointer wraps around]
```

---

## 6. Ring Buffer — Why Lock-Free, SPSC vs MPMC, False Sharing

### Lock-Free SPSC Design (ring_buffer.h)

The SPSCRing<T, N> has exactly two atomic variables: `head_` (written by producer) and `tail_` (written by consumer). No other synchronization is needed for correctness because:

1. Producer writes to `buf_[h]` THEN does a release-store on `head_`.
2. Consumer does an acquire-load on `head_` to see if new data is available.
   The acquire/release pair creates a happens-before relationship: the consumer
   is guaranteed to see the payload write that preceded the release-store.

### Memory Order Reasoning

```
Producer:                           Consumer:
buf_[h] = val;                      t = tail_.load(relaxed);
head_.store(next_h, release);       h = head_.load(acquire);   ← synchronizes-with
                                    if (h != t): out = buf_[t]; ← sees buf_[h]
```

The `release`/`acquire` pair on `head_` ensures the data written to `buf_[h]`
by the producer is visible to the consumer after the consumer's `acquire` load
on `head_`. Without this, the CPU could reorder the store to `buf_[h]` to after
the store to `head_`, and the consumer could read stale data.

### SPSC vs MPMC Trade-offs

| Property | SPSC (ours) | MPMC (rte_ring default) |
|---|---|---|
| Producers | 1 | Many |
| Consumers | 1 | Many |
| Synchronization | 2 atomics, no CAS | CAS loop on head and tail |
| Cost per op (uncontended) | ~5-10ns | ~15-30ns |
| Use case | NIC PMD → single lcore | Multi-lcore packet passing |
| Our usage | producer thread → lcore | Not implemented |

For multi-queue DPDK (RSS — Receive Side Scaling), the NIC hashes flows to
multiple RX queues, each served by a dedicated lcore. Each (NIC queue, lcore)
pair is an SPSC relationship, so SPSC rings are optimal.

### False Sharing Prevention

Without `alignas(64)`:
```
[head_][tail_][buf_...] — head_ and tail_ share a 64-byte cache line
```
Every `head_.store()` by the producer invalidates the consumer's cache entry
for `tail_` (same cache line). Every `tail_.store()` by the consumer invalidates
the producer's cache entry for `head_`. Result: ~100-200ns cache coherence
traffic on every push/pop — worse than a mutex.

With `alignas(64)`:
```
[cache line 0: head_]
[cache line 1: tail_]
[cache lines 2+: buf_]
```
Producer exclusively owns cache line 0. Consumer exclusively owns cache line 1.
No false sharing. Each operation is 1-2 cache line accesses.

---

## 7. Comparison Table: Feature by Feature

| Feature | Socket Path | DPDK Path | Notes |
|---|---|---|---|
| Packet source | `std::queue` via mutex | `SPSCRing` (lock-free) | |
| Producer sync | `std::mutex` + `cv::notify_one()` | Atomic ring push | |
| Consumer wait | `cv::wait()` (futex sleep) | Busy-poll spin loop | |
| Packet alloc | `std::queue` node (heap) | `MbufPool` bump-ptr | |
| Per-packet timestamp | `Packet::now_ns()` each | One per burst | |
| Cache behavior | Copy pollutes PDR cache | Cache-warm PDR lookups | |
| CPU when idle | ~0% (thread sleeps) | 100% (spin) | Key trade-off |
| Latency source | CV wakeup (~5-50μs) | Ring empty check (~1ns) | |
| Throughput | ~200K-1M pps | ~1-10M pps (simulated) | Real DPDK: 10-40M |
| Memory model | Heap allocation | Pre-allocated pool | |
| Thread model | Producer + 1 worker | Producer + 1 lcore | |
| Burst processing | No (one at a time) | Yes (32 per pop_burst) | |
| Rule table | Shared `const RuleTable&` | Shared `const RuleTable&` | Same in both |
| Latency p99 | High (CV jitter) | Low (deterministic) | See benchmark |
