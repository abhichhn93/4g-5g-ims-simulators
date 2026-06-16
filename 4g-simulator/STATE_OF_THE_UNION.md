# 4G/5G/IMS Simulator — State of the Union

*Last updated: 2026-06-17*

## What This Project Demonstrates

A multi-protocol telecom simulation suite in Modern C++ and Go, covering:

| Simulator | Protocols | Key Features |
|-----------|-----------|--------------|
| **4G EPC** | S1AP (APER), NAS-EPS, GTPv2-C, Diameter S6a/Gx, GTPv1-U | Attach, TAU, S1 Handover, PCAP, Prometheus /metrics |
| **5G Core** | NGAP (JSON/N2), HTTP SBI, PFCP (N4) | Registration, PDU Session, SMF+UPF, NRF discovery |
| **IMS/VoLTE** | SIP RFC 3261, SDP IR.92, Diameter Cx/Rx | REGISTER, INVITE, BYE, 183/200, RTP placeholder |

---

## 4G EPC Simulator — Implemented Flows

### Phase 1-2: Basic Attach (complete)
- eNB→MME S1AP (real APER bytes, Wireshark-verified)
- NAS-EPS: Attach Request/Accept/Complete, Auth Request/Response, Security Mode Cmd/Complete
- GTPv2-C: Create Session Req/Rsp, Modify Bearer
- Diameter S6a: Authentication-Information-Request/Answer
- Diameter Gx: Credit-Control-Request/Answer (P-GW → PCRF)
- PCAP output: Wireshark decodes all frames as standard protocols

### Phase 3: Sharded UE Store + Metrics (complete)
- 64-bucket `shared_mutex` sharded map for concurrent UE context lookups
- `atomic<uint32_t>` for lock-free IP and TEID allocation
- Thread pool (8 workers) for BULK N concurrent attach submissions
- `Metrics` class: P50/P95/P99 attach latency, throughput/sec
- BULK N command: fan-out, wait-for-all with timeout

### Phase 4: TAU + S1 Handover (complete — 2026-06-17)
- **TAU command**: `TAU <ue_id>` → UE→TAU Request (UL NAS) → MME validates TAI → TAU Accept (DL NAS)
  - NAS-EPS buildTauRequest (0x48) + buildTauAccept (0x49) with T3412=54min
  - PCAP: UL/DL NAS Transport wrapping real NAS bytes
- **HO command**: `HO <ue_id>` → 7-step S1 Handover (TS 36.413 §8.4)
  - HandoverRequired → Request → Ack → Command → ENBStatusTransfer → MMEStatusTransfer → Notify → UEContextRelease
  - PFCP path switch via Modify Bearer after HandoverNotify
  - PCAP: all 9 S1AP messages with correct procedure codes (Wireshark names them correctly)
- **Prometheus /metrics** endpoint: raw TCP HTTP/1.1 on :9090, no external libs
  - Exposes: mme_attach_total, P50/P95/P99, throughput, registered_ues
  - `curl http://localhost:9090/metrics` → Prometheus text exposition format

### Logging Levels (preserved throughout)
- **BEGINNER**: story-level narration ("UE moved to new TA → MME validates")
- **ENGINEER**: 3GPP IE details, offsets, timer values, thread model
- **INTERVIEW**: Q&A pairs embedded in flow (C++ patterns + telecom spec)

### Commands
```
CR <n>      — Attach n UEs
BULK <n>    — Thread-pool parallel n attaches + latency metrics
TAU <ue>    — Tracking Area Update for UE (simulates mobility)
HO <ue>     — S1 Handover for UE (7-step intra-LTE HO)
MODE level  — BEGINNER / ENGINEER / INTERVIEW
STATUS      — Print all registered UEs
QUIT
```

### CLI Automation
```bash
# Scenario runner (YAML → CLI commands → mme_sim)
./scenario_runner scenarios/mobility_and_handover.yaml | ./mme_sim

# Python automation (captures output, asserts keywords)
python3 test_automation.py
```

---

## 5G Core Simulator — Implemented Flows

### Increment 1-2: Registration (complete)
- gNB → AMF: N2/NGAP RegistrationRequest (SUCI)
- AMF → UDM: Nudm_UEAuthentication_Get SBI (HTTP/1.1 + JSON)
- AMF → UDM: Nudm_SDM_Get (am-data)
- AMF → gNB: RegistrationAccept (5G-GUTI, Allowed NSSAI)
- NRF discovery: all nodes register on startup, AMF discovers UDM via NRF

### Increment 3: PDU Session / SMF / UPF (complete — 2026-06-17)
- **SMF node** (TCP :29502): `Nsmf_PDUSession_CreateSMContext` handler
  - UE IP allocation from 10.45.0.0/16 DNN pool
  - N4 PFCP Session Establishment Request → Response to UPF
  - NRF registration + UPF discovery
- **UPF node** (UDP :8805): PFCP server
  - Session Establishment Request (type=50) → Response (type=51)
  - PDR/FAR installation logged, PCAP written
  - Session Modification (type=52) + Deletion (type=54) handled
- **AMF**: discovers SMF via NRF, calls `callSmf()` after PDU Session Request
- **gNB**: `PDU <ueId> [sessId] [dnn]` command → full AMF→SMF→UPF flow
- **PCAP**: SBI frames (TCP/HTTP on port 80) + PFCP frames (UDP/8805)

### 5G Commands
```
REG <ueId>              — Full 5G Registration (SUCI→Auth→Accept)
PDU <ueId> [sid] [dnn]  — PDU Session Establishment (→SMF→UPF PFCP)
QUIT
```

---

## IMS/VoLTE Simulator — Implemented Flows

- P-CSCF + S-CSCF + HSS + MTAS in one binary (ims_server)
- SIP REGISTER → 401 Digest Challenge → 200 OK
- SIP INVITE → 100 Trying → 183 Session Progress (SDP) → 200 OK → ACK
- SIP BYE → 200 OK
- Diameter Cx interface (S-CSCF → HSS) for auth + profile
- PCAP: SIP over TCP with 3-way handshake, SDP in payload

### IMS Commands
```
MODE BEGINNER/ENGINEER/INTERVIEW
REG <n>     — Register n UEs
CALL A B    — UE A calls UE B
BYE         — Terminate active call
QUIT
```

---

## Docker & Kubernetes

### Docker Compose
| Simulator | File | Services |
|-----------|------|---------|
| 4G EPC | `4g-simulator/docker-compose.yml` | mme-sim + prometheus + grafana |
| 5G Core | `5g-simulator/docker-compose.yml` | nrf + udm + amf + smf + upf + gnb |
| IMS | `ims-simulator/docker-compose.yml` | ims-server + ue-sim |

```bash
# 4G with observability
cd 4g-simulator && docker-compose up --build
curl http://localhost:9090/metrics   # Prometheus text
open http://localhost:3000           # Grafana (admin/admin)

# 5G full stack
cd 5g-simulator && docker-compose up --build

# IMS
cd ims-simulator && docker-compose up --build
```

### Kubernetes (minikube)
```bash
# 4G with HPA + Prometheus
kubectl apply -f 4g-simulator/k8s/
kubectl get hpa

# 5G: all nodes (NRF/UDM/AMF/SMF/UPF/gNB)
kubectl apply -f 5g-simulator/k8s/

# IMS: server + UE Job
kubectl apply -f ims-simulator/k8s/

# Trigger pod crash demo
kubectl delete pod -l app=mme-sim
kubectl get pods -w  # watch self-heal
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
| Observer (future) | PCRF→P-GW RAR → dedicated bearer |

---

## PCAP Guide

| Protocol | File | Wireshark filter |
|----------|------|-----------------|
| S1AP (real APER) | `mme_capture.pcap` | `s1ap` |
| NAS-EPS | (embedded in S1AP) | `nas-eps` |
| GTPv2-C | `mme_capture.pcap` | `gtpv2` |
| Diameter | `mme_capture.pcap` | `diameter` |
| 5G SBI (HTTP) | `5g_capture.pcap` | `http` |
| PFCP | `5g_smf_capture.pcap` / `5g_upf_capture.pcap` | `pfcp` |
| SIP | `ims_capture.pcap` | `sip` |
| SDP | (embedded in SIP INVITE) | `sdp` |
