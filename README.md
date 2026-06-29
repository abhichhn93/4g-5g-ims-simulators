# 4G + IMS/VoLTE + 5G Core Network Simulators (C++17)

Four from-scratch simulations of mobile core networks — **4G EPC**, **IMS/VoLTE**,
**5G Core (Service-Based Architecture)**, and a **CBRS Domain Proxy** —
written in C++17 using raw sockets, multithreading, and real protocol encoding.

A hands-on reference implementation for engineers studying 3GPP protocols,
C++ systems programming, and cloud-native telecom infrastructure.

Each project is independent (own CMake build, own Docker/K8s setup, own
docs) but they share subscriber numbering and design patterns, so together
they cover one continuous "UE attaches to 4G -> registers for VoLTE -> 5G
core equivalent" story across three generations of mobile core architecture.

| Project | What it is | Protocols |
|---|---|---|
| [`4g-simulator/`](4g-simulator/) | 4G EPC: eNB, MME, HSS, S-GW, P-GW, PCRF | S1AP + NAS-EPS (real ASN.1 PER), GTPv2-C, Diameter S6a/Gx |
| [`ims-simulator/`](ims-simulator/) | IMS/VoLTE: P-CSCF, I-CSCF, S-CSCF, MTAS, MRFC/MRFP, IMS-HSS | SIP + SDP, Diameter Cx/Rx, H.248 |
| [`5g-simulator/`](5g-simulator/) | 5G Core SBA: gNB, AMF, UDM, NRF | SBI/HTTP+JSON (Nudm/Nnrf per TS 29.5xx) |
| [`cbrs-domain-proxy/`](cbrs-domain-proxy/) | CBRS: Domain Proxy + SAS stub + CBSD agent | WinnForum WINNF-TS-0016, JSON/TCP |

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
cd 4g-simulator       && cmake -B build && cmake --build build
cd ims-simulator      && cmake -B build && cmake --build build
cd 5g-simulator       && cmake -B build && cmake --build build
cd cbrs-domain-proxy  && cmake -B build && cmake --build build
```

### Run CBRS (3 terminals)
```bash
# Terminal 1 — start SAS first
./cbrs-domain-proxy/build/sas_stub

# Terminal 2 — start Domain Proxy
./cbrs-domain-proxy/build/domain_proxy

# Terminal 3 — run a CBSD device
./cbrs-domain-proxy/build/cbsd_agent 1
# then type: REGISTER → GRANT → HEARTBEAT → RELINQUISH → DEREGISTER
```

See each subfolder's `README.md` for run order, CLI commands, and how to
open the generated pcap in Wireshark.
