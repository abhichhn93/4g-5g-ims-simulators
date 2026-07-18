# Interview Q&A: DPDK and 5G UPF

20 interview Q&A pairs covering UPF architecture, PFCP, DPDK, GTP-U, and QoS.

---

**Q1: What is the UPF and what does it do?**

The UPF (User Plane Function) is the 5G Core element that forwards actual user traffic between the radio access network (via the gNB on N3) and the internet (on N6). For uplink packets, it strips the GTP-U encapsulation, looks up forwarding rules (PDR/FAR), applies QoS enforcement, and forwards the inner IP packet. For downlink, it receives plain IP from the internet, looks up the UE's GTP-U tunnel, re-encapsulates, and sends to the gNB. In our simulator, `socket_path.h` and `dpdk_path.h` both implement this same PDR/FAR/QER pipeline — the difference is how packets are received and dispatched.

---

**Q2: What is PFCP and which node controls the UPF?**

PFCP (Packet Forwarding Control Protocol, 3GPP TS 29.244) is the N4 protocol between the SMF (Session Management Function) and the UPF. It follows the CUPS (Control/User Plane Separation) principle: the SMF is the brain (decides routing, QoS), and the UPF is the forwarding engine. The SMF programs the UPF via PFCP Session Establishment/Modification/Deletion Requests, which carry PDR, FAR, QER, and URR (Usage Reporting Rules). In our simulator, `build_demo_rules()` in `pfcp_rules.h` simulates what the SMF would have sent over N4.

---

**Q3: What are PDR, FAR, and QER and how do they work?**

A **PDR (Packet Detection Rule)** classifies an incoming packet. For UL it matches the GTP-U TEID; for DL it matches the UE's IP address. A **FAR (Forwarding Action Rule)** is referenced by the matched PDR and says what to do: FORWARD (with optional GTP-U encapsulation for DL), DROP, or BUFFER (during handover). A **QER (QoS Enforcement Rule)** is also referenced by the PDR and enforces MBR (Maximum Bit Rate) and GBR (Guaranteed Bit Rate) per QoS flow using a token-bucket policer. In our `RuleTable` class, all three are stored in hash maps and looked up in sequence per packet: PDR first (O(1) by TEID or UE-IP), then QER, then FAR.

---

**Q4: What is DPDK and why is it used in UPF?**

DPDK (Data Plane Development Kit) is a set of libraries for bypassing the Linux kernel's network stack entirely. Instead of interrupt-driven, syscall-based packet reception, DPDK uses a Poll Mode Driver (PMD) that busy-polls the NIC's RX descriptor ring directly from userspace. For a 5G UPF, this matters because the N3 interface can carry millions of GTP-U packets per second — far more than a kernel-socket UPF can handle (~200K-1M pps per core). DPDK-based UPFs achieve 10-40 Mpps per core by eliminating interrupts, syscalls, mutex operations, and per-packet memory allocation. Our `DpdkPath` class models this: lock-free ring, burst polling, pre-allocated mbuf pool.

---

**Q5: What is a Poll Mode Driver (PMD)?**

A PMD is a userspace NIC driver that continuously polls the NIC's hardware RX descriptor ring instead of waiting for interrupts. When rte_eth_rx_burst() is called, the PMD reads the NIC's tail pointer, copies metadata from filled descriptors into rte_mbuf pointers, and returns immediately with 0-N packets. The NIC DMA-fills the mbufs asynchronously. This eliminates the ~1-5μs interrupt latency and the context switch between IRQ handler and application thread. In our simulation, `SPSCRing::pop_burst()` in `ring_buffer.h` plays the role of rte_eth_rx_burst() — it returns 0-N entries without blocking.

---

**Q6: What are hugepages and why do they matter?**

Hugepages are memory pages larger than the default 4KB — typically 2MB or 1GB on x86. The CPU's TLB (Translation Lookaside Buffer) is a small cache (128-1024 entries) that maps virtual page addresses to physical addresses. At 4KB pages and 1Mpps with 1400-byte packets, you need ~350 TLB entries just for packet data, and TLB misses add ~100-200ns per miss. With 2MB hugepages, each entry covers 512 times more memory, so the same 1Mpps stream uses only ~1 TLB entry for packet data. DPDK requires hugepages for its mempool and ring buffers. Our simulator uses std::vector (regular heap) — a simplification that somewhat reduces the performance gap vs production DPDK.

---

**Q7: What is GTP-U and where does it run?**

GTP-U (GPRS Tunneling Protocol — User Plane, 3GPP TS 29.281) is the encapsulation protocol used on the N3 interface between the gNB and UPF. UL packets arrive as: Outer Ethernet → Outer IP (gNB→UPF) → Outer UDP (port 2152) → GTP-U header (4-12 bytes including TEID) → Inner IP (UE's packet). The UPF extracts the TEID from the GTP-U header to identify which UE/PDU session the packet belongs to. For DL, the UPF does the reverse: wrap the UE's inner IP packet in GTP-U with the appropriate TEID, then send to the gNB's N3 IP. In `pfcp_rules.h`, FAR.encap_gtpu=true indicates this DL re-encapsulation is needed.

---

**Q8: What is the difference between N3, N4, and N6?**

N3 is the interface between the gNB (base station) and the UPF carrying GTP-U encapsulated user data — this is the "radio side" of the UPF. N4 is the control interface between SMF and UPF using PFCP — used exclusively to program forwarding rules (PDR/FAR/QER) and receive usage reports; no user data flows on N4. N6 is the interface between the UPF and the internet (or data network) carrying plain IP packets — this is the "internet side." The UPF's core job is to bridge N3 and N6 while enforcing rules programmed via N4. DPDK is used only on N3 and N6 (high-rate data); N4 uses a normal Linux socket.

---

**Q9: How does the UPF get programmed per-UE session?**

When a UE requests a PDU session, the AMF triggers the SMF via N11. The SMF fetches the subscriber's QoS profile from the UDM via N10, then sends a PFCP Session Establishment Request to the UPF on N4. This request contains: one or more PDRs (UL and DL, keyed by TEID and UE-IP), FARs (what to do: forward to N6 or encap to N3), QERs (MBR/GBR per QFI), and optionally URRs (how to report usage). The UPF installs these into its fast-path tables (in our case, `RuleTable::teid_to_pdr_` and `ueip_to_pdr_`). The SMF then sends the F-TEID (UPF's allocated TEID) to the gNB via AMF on N2, so the gNB knows what TEID to use in UL GTP-U packets.

---

**Q10: What is TEID and why is it needed?**

TEID (Tunnel Endpoint Identifier) is a 32-bit value in the GTP-U header that identifies a specific GTP-U tunnel — which corresponds to one UE's PDU session on one N3 link. Multiple UEs share the same N3 physical link between a gNB and UPF, so the TEID is how the UPF distinguishes whose traffic is whose. The gNB allocates the UL TEID (used in UL packets from gNB → UPF); the UPF allocates the DL TEID (used in DL packets from UPF → gNB). These are exchanged during PDU session setup via N2. In our `pfcp_rules.h`, the PDR.teid field is the UL TEID and is the primary key for the `teid_to_pdr_` hash map.

---

**Q11: What is the difference between CUPS and non-CUPS UPF architecture?**

**Non-CUPS** (legacy 4G EPC): The P-GW (Packet Data Network Gateway) combines control-plane and data-plane in one process. Scaling means scaling both together, which is wasteful and inflexible. **CUPS (Control/User Plane Separation)** was introduced in 3GPP Release 14 for 4G and is native to 5G: the SMF (control) and UPF (user plane) are separate network functions communicating via N4/PFCP. This allows independent scaling — you can add UPF capacity without touching SMF, and you can move UPF instances geographically close to the radio (MEC/edge) while SMF stays centralized. Our simulator models CUPS: `pfcp_rules.h` is the rule store that the SMF would program (via `build_demo_rules()`), and `socket_path.h`/`dpdk_path.h` are the UPF's forwarding engines.

---

**Q12: How would you scale a UPF horizontally?**

Horizontal UPF scaling has three approaches. First, **per-UE session affinity**: the SMF assigns each new PDU session to a specific UPF instance (e.g., by consistent hashing on SUPI or gNB ID). Each UPF handles a subset of UEs independently. Second, **N9 (UPF chaining)**: multiple UPF instances can be chained — an "anchor UPF" handles N6 while "branching UPF" instances handle N3, connected via N9 (another GTP-U interface). This supports ULCL (Uplink Classifier) for traffic steering. Third, **RSS (Receive Side Scaling)** within one UPF: the NIC hashes flows to multiple RX queues, each processed by a dedicated DPDK lcore. Real DPDK UPFs like OAI-UPF scale to 4-8 lcores this way.

---

**Q13: What is ULCL and when is it used?**

ULCL (Uplink Classifier) is a UPF capability defined in 3GPP TS 23.501. An ULCL UPF inspects UL packets and routes them to different data networks based on prefix matching — for example, traffic to 10.0.0.0/8 goes to a private enterprise network while traffic to the public internet goes to the anchor UPF via N9. ULCL is used in MEC (Multi-Access Edge Computing) scenarios where local traffic (to an edge data center) should be offloaded at a nearby UPF without going all the way to the central UPF. The SMF programs the ULCL UPF with additional PDRs/FARs that implement the traffic steering rules. Our simulator doesn't implement ULCL but the `FAR.dst_ip` field is where the routing target would be configured.

---

**Q14: How does QoS work in the UPF?**

5G QoS is per-QoS Flow (identified by QFI, 6-bit). The SMF programs a QER for each QFI with MBR (Maximum Bit Rate) and optionally GBR (Guaranteed Bit Rate). In the UPF fast path, after PDR match, the QER is applied using a token-bucket policer: tokens accumulate at the MBR rate; each packet consumes tokens equal to its byte length; if insufficient tokens, the packet is dropped or marked (DSCP remarking). For GBR flows (voice, video), the UPF must reserve resources to guarantee the GBR rate even when the network is congested. In our simulator, `QER` structs have all these fields but we don't enforce the token bucket — the lookup happens but policing is elided. In real DPDK UPFs: `rte_meter_trtcm` (Two Rate Three Color Marker per RFC 4115).

---

**Q15: What is the difference between GBR and non-GBR flows?**

**GBR (Guaranteed Bit Rate)** flows — like 5QI=1 (voice) and 5QI=2 (video) — are allocated dedicated resources. The network guarantees their bit rate even under congestion. The UPF must reserve capacity for GBR flows, and the RAN (gNB) allocates dedicated radio resources. GBR flows have both MBR and GBR configured in the QER. **Non-GBR** flows (like 5QI=9, best-effort data) only have an MBR — they get whatever capacity is left after GBR flows are served. If the network is congested, non-GBR packets are dropped or delayed. In `build_demo_rules()`, QFI=1 (voice) and QFI=2 (video) have non-zero `gbr_ul/dl_kbps`; QFI=9 (data) has zero GBR.

---

**Q16: What observability would you add to a production UPF?**

A production UPF needs several layers of observability. At the PFCP level: Usage Reporting Rules (URRs) programmed by the SMF that trigger when byte/packet thresholds are exceeded — the UPF sends Session Report Requests back to the SMF for charging and analytics. At the network function level: Prometheus metrics exposed via HTTP — `upf_rx_packets_total`, `upf_tx_packets_total`, `upf_dropped_total{reason="no_pdr"}`, `upf_latency_ns_bucket` (histogram). At the DPDK level: `rte_eth_stats_get()` for NIC-level stats (rx_good_packets, tx_good_packets, rx_missed_errors), plus per-lcore packet rates. In our simulator, `PathStats` provides `pkts_rx`, `pkts_forwarded`, `pkts_dropped`, `pkts_no_pdr`, and latency percentiles — the production equivalents would be Prometheus-exported.

---

**Q17: What would you change to make our simulator production-grade?**

Five main gaps. First, replace `SPSCRing` with real DPDK `rte_ring` and `rte_eth_rx_burst()` against an actual NIC PMD (mlx5 or i40e). Second, use hugepages for the mbuf pool (`rte_pktmbuf_pool_create` instead of `std::vector`). Third, implement the token-bucket policer in `process_burst()` using `rte_meter_trtcm` for actual QER enforcement. Fourth, add RCU-style rule updates so the SMF can modify PDR/FAR tables via N4 PFCP without stalling the lcore — currently `RuleTable` is immutable after startup. Fifth, implement GTP-U parsing (strip outer UDP/IP/GTP-U header on UL, prepend on DL) instead of treating the TEID as pre-parsed metadata.

---

**Q18: How does handover affect the UPF?**

During an Xn handover (between two gNBs with Xn interface), the UE moves from source gNB to target gNB. The key UPF impact: the DL GTP-U tunnel must switch from source gNB N3 IP to target gNB N3 IP. The SMF sends a PFCP Session Modification Request to the UPF with an updated FAR (new DL tunnel TEID and target gNB IP). During the handover window, the UPF may briefly forward DL packets to both source and target gNB (data forwarding tunnel). For handovers via N2 (Ng-Handover, e.g., between different gNBs without Xn), the AMF coordinates with SMF which then updates the UPF. Our simulator doesn't implement handover but it's the primary reason `FAR` has modifiable `dst_ip` and `dst_port` fields.

---

**Q19: What is a PFCP Session Modification used for?**

PFCP Session Modification Request (sent by SMF to UPF) is used for: (1) handover — update DL FAR with new target gNB N3 address and TEID; (2) QoS change — update QER parameters (MBR/GBR) when the UE requests a different QoS profile; (3) policy change — update FAR action (e.g., DROP → FORWARD when UE moves from idle to connected, or FORWARD → BUFFER when UE becomes unreachable); (4) URR update — add or modify Usage Reporting Rules for online/offline charging; (5) PDU session anchor change (UPF relocation). In our simulator, `RuleTable` is populated once at startup via `build_demo_rules()`. A production simulator would have a PFCP state machine that applies Session Modification Requests to the live `RuleTable`.

---

**Q20: How does DPDK handle multiple cores (RSS and multi-queue)?**

RSS (Receive Side Scaling) is a NIC feature that hashes each incoming packet's flow identifier (typically src/dst IP + src/dst port, or GTP-U TEID) to one of N RX queues. Each RX queue is polled by a dedicated DPDK lcore on its own CPU core. This achieves linear throughput scaling: 4 lcores = ~4x throughput of 1 lcore, as long as the NIC has enough queues and the hashing distributes flows evenly. In DPDK code: `rte_eth_dev_configure(port, num_rx_queues, num_tx_queues, &port_conf)` configures multiple queues; each lcore calls `rte_eth_rx_burst(port, queue_id, ...)` on its assigned queue. For a 5G UPF with TEID-based hashing, packets from the same UE always land on the same lcore — ensuring in-order delivery and avoiding cross-lcore rule lookups. Our simulator has a single `SPSCRing` (single lcore) — the `--workers` argument is a placeholder for this multi-queue extension.
