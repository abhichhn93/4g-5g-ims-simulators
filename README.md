# 4G + IMS/VoLTE + 5G Core Network Simulators (C++17)

Three from-scratch simulations of mobile core networks — **4G EPC**, **IMS/VoLTE**,
and **5G Core (Service-Based Architecture)** — written in C++17 using raw
sockets, multithreading, and real protocol encoding. Built by an ex-Samsung
4G/5G core network engineer (8 years C++) as a hands-on refresher and as a
learning resource for engineers/students studying telecom protocols.

Each project is independent (own CMake build, own Docker/K8s setup, own
docs) but they share subscriber numbering and design patterns, so together
they cover one continuous "UE attaches to 4G -> registers for VoLTE -> 5G
core equivalent" story across three generations of mobile core architecture.

| Project | What it is | 3GPP protocols |
|---|---|---|
| [`4g-simulator/`](4g-simulator/) | 4G EPC: eNB, MME, HSS, S-GW, P-GW, PCRF | S1AP + NAS-EPS (hand-written ASN.1 PER), GTPv2-C, Diameter S6a/Gx |
| [`ims-simulator/`](ims-simulator/) | IMS/VoLTE: P-CSCF, S-CSCF, MTAS, IMS-HSS, PCRF | SIP + SDP, Diameter Cx/Gx |
| [`5g-simulator/`](5g-simulator/) | 5G Core SBA: gNB, AMF, UDM, NRF | SBI/HTTP+JSON (Nudm/Nnrf per TS 29.5xx) |

Each subfolder has its own `README.md` with build/run instructions, an
architecture diagram, and a `docs/INTERVIEW_QA.md` with a word-for-word
interview script for that protocol flow.

## What's real vs. simplified

The pcaps captured by each simulator contain **genuine protocol bytes**:
S1AP is real ASN.1 PER (byte-for-byte matches 3GPP, verified against
Wireshark's native S1AP dissector), SIP/HTTP are real text/JSON, GTPv2 and
Diameter are real RFC-correct headers. A small Lua dissector
(`4g-simulator/mme_sim_dissector.lua`, reused by `ims-simulator`) is the only
non-stock addition needed for Diameter.

Known, documented simplifications: SCTP is emulated over TCP (S1AP/N2 — macOS
doesn't support SCTP easily), the 5G N2 interface is plain TCP+JSON rather
than NGAP, NRF keeps one instance per NF type, and 5G-AKA uses byteXor
instead of full Milenage. Each project's own README/docs spell these out.

## Cross-cutting C++ / systems topics

- STL: `map`/`unordered_map`/`vector`/`shared_ptr`/`unique_ptr` throughout
- Concurrency: `mutex`, sharded `shared_mutex`, `condition_variable`, thread
  pool, atomics
- Design patterns: Flyweight (subscriber profiles), RAII (sockets)
- DevOps: multi-stage Dockerfiles, docker-compose, Kubernetes Deployments/
  Services/ConfigMaps, readiness/liveness probes, minikube, horizontal
  scaling + self-healing demos
- Testing: pytest + tshark-based pcap assertions (`5g-simulator/tests/`)

## Build

Each project builds independently:

```
cd 4g-simulator && mkdir build && cd build && cmake .. && make
cd ims-simulator && mkdir build && cd build && cmake .. && make
cd 5g-simulator && mkdir build && cd build && cmake .. && make
```

See each subfolder's `README.md` for run order, CLI commands, and how to
open the generated pcap in Wireshark.
