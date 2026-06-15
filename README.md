# 5G Core Simulator in C++17

A from-scratch simulation of a **5G Core (5GC) Service-Based Architecture**
— gNB, AMF, UDM, and NRF talking real HTTP/1.1 + JSON over the Service-Based
Interface (SBI), built in C++17 using raw TCP sockets and multithreading.

> Sibling project to [`../mme-simulator/`](../mme-simulator/) (4G EPC) and
> [`../ims-simulator/`](../ims-simulator/) (IMS/VoLTE). Subscriber numbering
> matches the 4G sim — UE #1 is `imsi-404100000000001` on both sides.

---

## What It Implements

```
[gNB] ──N2 (TCP, JSON)──► [AMF] ──SBI/HTTP+JSON──► [UDM]
                              │                        │
                              └──────► [NRF] ◄─────────┘
                       (Nnrf_NFManagement / Nnrf_NFDiscovery, TS 29.510)
```

| Node | Port    | What it does |
|------|---------|--------------|
| gNB  | (N2 client) | Simulated UE + gNB, sends RegistrationRequest over N2 |
| AMF  | 38412 (N2) | Access & Mobility Management — N2 server, SBI client to UDM/NRF, writes `5g_capture.pcap` |
| UDM  | 29503 (SBI) | Unified Data Management — Nudm_UEAuthentication_Get, Nudm_SDM_Get |
| NRF  | 29510 (SBI) | NF Repository Function — register/discover (Nnrf_NFManagement, Nnrf_NFDiscovery) |

Full **Registration** flow (TS 23.502 §4.2.2.2, simplified 5G-AKA):
`SUCI -> RegistrationRequest -> Nudm_UEAuthentication_Get ->
AuthenticationRequest/Response -> Nudm_SDM_Get -> RegistrationAccept
(5G-GUTI + NSSAI) -> RegistrationComplete`.

Every NF registers itself with the NRF on startup, and AMF discovers UDM via
the NRF (`Nnrf_NFDiscovery_Search`) instead of a hardcoded address — the
"how do microservices find each other" story in 5G SBA.

---

## Build & Run

### Prerequisites
Same toolchain as `mme-simulator` — clang/g++ + cmake, C++17.

### Build
```bash
cd 5g-simulator
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```
Produces four binaries: `nrf_sim`, `udm_sim`, `amf_sim`, `gnb_sim`.

### Run (4 terminals, in this order — NRF first)
```bash
# Terminal 1
./nrf_sim

# Terminal 2
./udm_sim

# Terminal 3
./amf_sim

# Terminal 4 — interactive
./gnb_sim
```

`gnb_sim` commands: `REG <n>` (register UE n) / `QUIT`.

`LOG_LEVEL=ALL ./amf_sim` (or any binary) shows the full story narration plus
raw JSON wire dumps — the most "what message went where" verbose mode.

---

## Docker / Kubernetes

```bash
docker compose up --build      # 4 containers: nrf, udm, amf, gnb
```

`k8s/` has Deployments + Services for all four nodes (`g5-nrf`, `g5-udm`,
`g5-amf` x2 replicas, `g5-gnb` x2 replicas) plus a `plmn-configmap.yaml` for
runtime MCC/MNC. Verified on minikube: 6 pods, readiness/liveness probes,
horizontal scaling, self-healing (`kubectl delete pod`), and a real
NRF-discovery race between the two AMF replicas (one fell back to its env
var, one used NRF — both code paths exercised live).

```bash
eval $(minikube docker-env)
for t in nrf udm amf gnb; do docker build -t g5-$t:latest --target $t .; done
kubectl apply -f k8s/
kubectl get pods
```

---

## Capture Packets / Wireshark

Only `amf_sim` writes `5g_capture.pcap` (NRF traffic is visible in the
per-node session logs instead — `g5_nrf_session.log`, etc.)

```bash
tshark -r build/5g_capture.pcap -T fields -e _ws.col.Protocol | sort | uniq -c
```

SBI traffic (AMF<->UDM) dissects natively as **HTTP / HTTP-JSON** — real
`Nudm_UEAuthentication_Get` / `Nudm_SDM_Get` URIs and JSON bodies. N2
(gNB<->AMF) shows as **TCP** carrying length-prefixed JSON — a documented
simplification (real N2 is NGAP over SCTP; SCTP isn't practical on macOS).

---

## Tests

```bash
cd tests && pip install -r requirements.txt
pytest test_registration.py -v
```
11 tests, every assertion derived from a real tshark-decoded pcap (not
guessed values).

---

## Project Structure
```
5g-simulator/
├── src/
│   ├── common/
│   │   ├── wire.h           HTTP build/parse + flat-JSON helpers
│   │   ├── aka_lite.h        Simplified 5G-AKA (byteXor instead of Milenage)
│   │   ├── nrf_client.h      registerSelf() / discover() — shared by UDM+AMF
│   │   ├── ids5g.h            SUPI/SUCI/PLMN helpers (env-configurable PLMN)
│   │   ├── socket_wrapper.h  RAII TCP socket
│   │   ├── logger.h           Color-coded logger (LOG_LEVEL env var)
│   │   └── pcap_writer.*      Pcap capture writer (HTTP framing)
│   ├── nrf/nrf_main.cpp      NRF — NF register/discover, port 29510
│   ├── udm/udm_main.cpp      UDM — SBI server, port 29503
│   ├── amf/amf_main.cpp      AMF — N2 server :38412 + SBI client
│   └── gnb/gnb_main.cpp      gNB + simulated UE, N2 client (CLI)
├── tests/test_registration.py
├── docker/                   per-node entrypoint scripts
├── k8s/                       Deployments + Services + PLMN ConfigMap
├── docker-compose.yml
├── Dockerfile                 4-stage multi-target build (nrf/udm/amf/gnb)
└── CMakeLists.txt
```

---

## Related 3GPP Standards

| Standard | What it covers |
|----------|---------------|
| TS 23.501 / 23.502 | 5G System architecture & procedures |
| TS 29.510 | Nnrf — NF Management & Discovery |
| TS 29.503 | Nudm — Unified Data Management |
| TS 33.501 | 5G-AKA security (simplified here as byteXor) |

> 4G EPC standards are covered in [`../mme-simulator/`](../mme-simulator/),
> IMS/VoLTE in [`../ims-simulator/`](../ims-simulator/).

---

## Roadmap

- **Increment 3**: SMF + UPF + PFCP/N4 — PDU Session Establishment (UE gets
  an IP, the 5G analogue of "UE gets IP from P-GW" in the 4G sim)
- **Increment 4**: Network slicing — multiple S-NSSAI (eMBB/URLLC/mMTC)
- **Increment 5**: Docker/K8s for SMF/UPF

---

## Author

Built as a deep-dive into 5G Core / Service-Based Architecture — combining
8 years of production experience in 4G/5G core networks with modern C++17
to create a fully educational, open-source simulator.
