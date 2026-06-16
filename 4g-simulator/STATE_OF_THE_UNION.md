# 4G/5G/IMS Simulator — State of the Union

*Last updated: 2026-06-17*

## What This Project Demonstrates

A multi-protocol telecom simulation suite in Modern C++ covering three full stacks:

| Simulator | Protocols | Key Features |
|-----------|-----------|--------------|
| **4G EPC** | S1AP (APER), NAS-EPS, GTPv2-C, Diameter S6a/Gx | Attach, TAU, S1 HO, PCAP, Prometheus, CHAOS, O-RAN E2 |
| **5G Core** | NGAP (JSON/N2), HTTP SBI, PFCP (N4) | Registration, PDU Session, NSSAI slicing, NRF, SMF+UPF |
| **IMS/VoLTE** | SIP RFC 3261, SDP IR.92, Diameter Cx/Rx | REGISTER, INVITE, BYE, Conference, Call Barring |

---

## 4G EPC Simulator — Implemented Flows

### Basic Attach (complete)
- eNB→MME S1AP (real APER bytes, Wireshark-verified)
- NAS-EPS: Attach Request/Accept/Complete, Auth Request/Response, Security Mode Cmd/Complete
- GTPv2-C: Create Session Req/Rsp, Modify Bearer
- Diameter S6a: Authentication-Information-Request/Answer
- Diameter Gx: Credit-Control-Request/Answer (P-GW → PCRF)
- PCAP output: Wireshark decodes all frames as standard protocols

### Sharded UE Store + Metrics (complete)
- 64-bucket `shared_mutex` sharded map for concurrent UE context lookups
- `atomic<uint32_t>` for lock-free IP and TEID allocation
- Thread pool (8 workers) for BULK N concurrent attach submissions
- `Metrics` class: P50/P95/P99 attach latency, throughput/sec
- Prometheus /metrics endpoint: HTTP on :9090, no external libs

### TAU + S1 Handover (complete)
- `TAU <ue_id>` — 3-step Tracking Area Update (TS 24.301 §5.5.3)
- `HO <ue_id>` — 7-step S1 Handover (TS 36.413 §8.4), Modify Bearer path switch
- PCAP: all 9 S1AP messages with correct procedure codes

### JSON Event Logger (Phase 2a — complete)
- Every key protocol message also written to `sim_events.jsonl`
- Each line: `{ts, from, to, msg, interface, port, hex, sim, beginner_text, interview_q, interview_a}`
- Consumed by `tools/viz_server.py` for live visualization

### CHAOS MODE (Phase 4a — complete)
- `CHAOS ON` → 20% probability of dropping/corrupting one message per flow
- 4G: drops Diameter AIA → auth timeout scenario with recovery narration
- All chaos events logged at BEGINNER/ENGINEER/INTERVIEW levels

### O-RAN E2 Interface Stub (Phase 5c — complete)
- `./build/e2_agent` — eNB E2 termination (port 36421)
- `./build/xapp_sim [ueId] [mcs]` — Near-RT RIC xApp
- Flow: E2SetupRequest → E2SetupResponse → RICControlRequest → RICControlAck
- INTERVIEW narration: what O-RAN is, Near-RT vs Non-RT RIC, MCS control

### Commands
```
CR <n>        — Attach n UEs
BULK <n>      — Thread-pool parallel n attaches + latency metrics
TAU <ue>      — Tracking Area Update for UE (simulates mobility)
HO <ue>       — S1 Handover for UE (7-step intra-LTE HO)
MODE level    — BEGINNER / ENGINEER / INTERVIEW
CHAOS on|off  — Toggle 20% random fault injection
STATUS        — Print all registered UEs
QUIT
```

---

## 5G Core Simulator — Implemented Flows

### Registration (complete)
- gNB → AMF: N2/NGAP RegistrationRequest (SUCI)
- AMF → UDM: Nudm_UEAuthentication_Get + Nudm_SDM_Get (SBI HTTP/1.1)
- AMF → gNB: RegistrationAccept (5G-GUTI, Allowed NSSAI)
- NRF discovery: all nodes register on startup, AMF discovers UDM/SMF via NRF

### PDU Session / SMF / UPF (complete)
- **SMF** (TCP :29502): `Nsmf_PDUSession_CreateSMContext` → IP allocation → PFCP
- **UPF** (UDP :8805): PFCP Session Est/Mod/Del, PDR+FAR installation
- **AMF**: discovers SMF via NRF, calls `callSmf()` after PDU Session Request
- **gNB**: `PDU <ueId> [sessId] [dnn]` → full AMF→SMF→UPF flow

### Network Slicing (Phase 4d — complete)
- `REG <ueId> --slice embb|urllc` → NSSAI SST=1 or SST=2 in RegistrationRequest
- AMF logs slice selection with INTERVIEW narration (TS 23.501 §5.15)
- RegistrationAccept carries the actual Allowed NSSAI back to gNB/UE

### CHAOS MODE (Phase 4a — complete)
- `CHAOS ON` → 20% chance AMF drops UDM auth call → Registration Reject
- Recovery path narrated with retry/backoff INTERVIEW explanation

### 5G Commands
```
REG <ueId> [--slice embb|urllc]  — Full 5G Registration with NSSAI
PDU <ueId> [sid] [dnn]           — PDU Session (→SMF→UPF PFCP)
CHAOS on|off                     — Toggle fault injection
QUIT
```

---

## IMS/VoLTE Simulator — Implemented Flows

- P-CSCF + S-CSCF + HSS + MTAS in one binary (`ims_server` / `mme_ims`)
- SIP REGISTER → 401 Digest Challenge → 200 OK (Diameter Cx SAR/SAA)
- SIP INVITE → 100 Trying → 183 Session Progress (SDP IR.92) → 200 OK → ACK
- SIP BYE → 200 OK + Diameter Rx STR to PCRF → QCI=1 bearer released
- PCAP: real SIP over TCP (port 5060), SDP in payload, Diameter on 3868

### CHAOS MODE (Phase 4a — complete)
- `CHAOS ON` → 20% chance SIP 200 OK INVITE is corrupted
- P-CSCF retransmission scenario narrated with RFC 3261 §17 explanation

### IMS Commands
```
MODE BEGINNER/ENGINEER/INTERVIEW
REG A|B|C|ALL  — Register UE(s)
CALL A B       — VoLTE call flow (INVITE/183/200/ACK)
CONF           — 3-party conference call
WAIT           — Call waiting demonstration
BARR           — Call barring demonstration
BYE            — Terminate active call
CHAOS on|off   — Toggle fault injection
STATUS         — Print registration state
QUIT
```

---

## Visualization & Tools

### Live Ladder Diagram Visualizer (Phase 2b/2c — complete)
```bash
pip install websockets pyyaml
python3 tools/viz_server.py     # WebSocket :8765 + HTTP :8080
# Open browser: http://localhost:8080
```
- 3 tabs: **BEGINNER** (plain English) | **ENGINEER** (hex + field decode) | **INTERVIEW** (Q&A cards)
- Live animated arrows as events arrive from `sim_events.jsonl`
- Auto-matches events to interview questions via `tag_rules.json`

### One-Command Demo (Phase 2d — complete)
```bash
bash tools/run_demo.sh 4g    # starts 4G sim + viz + runs scenario + opens browser
bash tools/run_demo.sh 5g    # same for 5G
bash tools/run_demo.sh ims   # same for IMS
```

### Offline Quiz CLI (Phase 3b — complete)
```bash
python3 tools/quiz.py --list-domains          # 465 questions across 11 domains
python3 tools/quiz.py --domain cpp --count 5  # 5 random C++ questions
python3 tools/quiz.py --domain 4g --level senior
python3 tools/quiz.py --domain ims
```

### Python Scenario Runner (Phase 5d — complete)
```bash
python3 tools/run_scenario.py 4g-simulator/scenarios/basic_attach.yaml
python3 tools/run_scenario.py 4g-simulator/scenarios/mobility_and_handover.yaml --verbose
```

### PCAP Fault Analyzer (Phase 5e — complete)
```bash
python3 tools/pcap_analyzer.py 4g-simulator/mme_capture.pcap
python3 tools/pcap_analyzer.py 5g-simulator/5g_smf_capture.pcap
python3 tools/pcap_analyzer.py ims-simulator/ims_capture.pcap --ai
```
Detects: Diameter auth failure, GTPv2 rejection, SIP 4xx/5xx, PFCP errors, latency issues.

---

## Docker & Kubernetes

| Simulator | Docker Compose | Services |
|-----------|---------------|---------|
| 4G EPC | `4g-simulator/docker-compose.yml` | mme-sim + prometheus + grafana |
| 5G Core | `5g-simulator/docker-compose.yml` | nrf + udm + amf + smf + upf + gnb |
| IMS | `ims-simulator/docker-compose.yml` | ims-server + ue-sim |

```bash
cd 4g-simulator && docker-compose up --build
curl http://localhost:9090/metrics   # Prometheus text format
open http://localhost:3000           # Grafana dashboard (admin/admin)

cd 5g-simulator && docker-compose up --build
cd ims-simulator && docker-compose up --build
```

---

## C++ Patterns for Interviews

| Pattern | Where |
|---------|-------|
| Producer-Consumer | CLI→eNB command queue (`std::queue` + `mutex` + `cv`) |
| RAII | Socket RAII destructor closes fd; `shared_ptr<UeContext>` |
| Sharding | 64-bucket `shared_mutex` UE store (UeContextStore) |
| Thread Pool | BULK command, 8 workers, task queue + cv |
| Flyweight | `shared_ptr<SubscriberProfile>` shared across UEs |
| State Machine | EMM states per UE (DEREGISTERED→TAU_PENDING→REGISTERED) |
| Atomic counters | Lock-free TEID/IP allocation (`atomic<uint32_t>`) |
| Observer | Chaos mode callbacks; PCRF→P-GW RAR dedicated bearer |
| Strategy | CHAOS on/off toggling per-flow behavior at runtime |

---

## PCAP Guide

| Protocol | File | Wireshark filter |
|----------|------|-----------------|
| S1AP (real APER) | `mme_capture.pcap` | `s1ap` |
| NAS-EPS | (embedded in S1AP) | `nas-eps` |
| GTPv2-C | `mme_capture.pcap` | `gtpv2` |
| Diameter S6a/Gx | `mme_capture.pcap` | `diameter` |
| 5G SBI (HTTP) | `5g_capture.pcap` | `http` |
| PFCP (N4) | `5g_smf_capture.pcap` / `5g_upf_capture.pcap` | `pfcp` |
| SIP RFC 3261 | `ims_capture.pcap` | `sip` |
| SDP IR.92 | (embedded in SIP INVITE) | `sdp` |
| O-RAN E2 | (JSON over TCP, port 36421) | `tcp.port==36421` |

---

## GitHub Actions CI

| Workflow | Triggers | What it builds |
|----------|----------|----------------|
| `4g-build.yml` | push/PR to `4g-simulator/` | `mme_sim`, `scenario_runner`, `e2_agent`, `xapp_sim` |
| `5g-build.yml` | push/PR to `5g-simulator/` | `nrf_sim`, `udm_sim`, `amf_sim`, `gnb_sim`, `smf_sim`, `upf_sim` |
| `ims-build.yml` | push/PR to `ims-simulator/` | `ims_server`, `ue_sim`, `mme_ims` |
