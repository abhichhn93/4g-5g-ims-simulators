# IMS / VoLTE Simulator in C++17

A from-scratch simulation of the **IMS (IP Multimedia Subsystem)** — the SIP +
Diameter network that carries VoLTE voice calls — built in C++17 using raw
TCP sockets, multithreading, and real protocol message encoding (SIP text
messages, Diameter-style TLV).

> This is the sibling project to [`../4g-simulator/`](../4g-simulator/) (4G EPC)
> and [`../5g-simulator/`](../5g-simulator/) (5G core). A UE attaches to the 4G
> EPC first to get an IP address, then uses that IP to register with IMS here —
> see [Connection: 4G EPC ↔ IMS](#connection-4g-epc--ims) below.

---

## What It Implements

```
[UE] ──SIP:5060──► [P-CSCF] ──SIP:5070──► [S-CSCF+MTAS] ──Cx:3870──► [IMS-HSS]
```

| Node | Port | What it does |
|------|------|-------------|
| P-CSCF | 5060 | First SIP contact, Rx→PCRF for QCI=1 bearer |
| S-CSCF | 5070 | SIP registrar, invokes MTAS via ISC |
| MTAS   | 5070 | Service logic: call waiting, barring, conference |
| IMS-HSS| 3870 | Cx interface — subscriber profile + iFC |

Two ways to run it:

- **`mme_ims`** — single process, 3 UEs (A/B/C) driven from one CLI. Good for
  a quick demo and for generating one combined pcap.
- **`ims_server` + `ue_sim`** — multi-terminal mode. Run the server in one
  terminal and `./ue_sim A`, `./ue_sim B`, `./ue_sim C` in three more —
  closer to how a real client/server split looks.

---

## Build & Run

### Prerequisites

Same toolchain as `4g-simulator` — see
[`../4g-simulator/README.md`](../4g-simulator/README.md#prerequisites)
(clang/g++ + cmake, C++17).

### Build

```bash
cd ims-simulator
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Produces three binaries: `mme_ims`, `ims_server`, `ue_sim`.

### Run: single-process demo (`mme_ims`)

```bash
cd build
./mme_ims
```

**Commands:**
```
REG A|B|C|ALL  → IMS registration (SIP REGISTER → Cx SAR/SAA → 200 OK)
CALL A B       → VoLTE call A → B (INVITE → MTAS → 200 OK → Rx AAR → QCI=1 bearer)
CONF           → 3-party conference (A+B+C via MRFC/MRFP)
WAIT           → Call waiting scenario
BARR           → Call barring (603 Decline)
BYE            → End call (release QCI=1 bearer)
STATUS         → Show registered UEs
QUIT           → Shutdown (saves ims_capture.pcap)
```

### Run: multi-terminal mode (`ims_server` + `ue_sim`)

```bash
# Terminal 1 — start the server first
cd build && ./ims_server

# Terminals 2-4 — one UE each
cd build && ./ue_sim A
cd build && ./ue_sim B
cd build && ./ue_sim C
```

Each `ue_sim` accepts: `REG CALL ACCEPT REJECT HOLD RESUME VIDEO VOICE CONF BYE STATUS QUIT`.
This mode writes `ims_server_capture.pcap` plus one pcap per UE; merge them with:

```bash
./merge_pcap.sh   # -> build/ims_combined.pcap
```

---

## Capture Packets in Wireshark

```bash
sudo tcpdump -i lo0 'port 5060 or port 5070 or port 3870' -w ~/Desktop/ims_capture.pcap
```

**Key Wireshark filters:**
```
tcp.port == 5060  and tcp.len > 0   # SIP (UE ↔ P-CSCF ↔ S-CSCF)
tcp.port == 3870  and tcp.len > 0   # Diameter Cx (S-CSCF ↔ IMS-HSS)
```

---

## Project Structure

```
ims-simulator/
├── src/
│   ├── common/
│   │   ├── logger.h          Color-coded thread-safe logger
│   │   ├── visual_logger.h   Step-by-step call-flow banners
│   │   ├── socket_wrapper.h  RAII TCP socket (connect/accept/send/recv)
│   │   ├── tlv.h             Binary TLV message encoder/decoder
│   │   ├── message_types.h   Protocol message type enums
│   │   ├── exceptions.h       Custom exception types
│   │   └── pcap_writer.*      Pcap capture writer
│   ├── ims/
│   │   ├── sip.h              SIP message types + TLV encoding
│   │   ├── sip_text.h         Real SIP text-message formatting
│   │   ├── pcscf_node.*       P-CSCF: SIP proxy + Rx interface
│   │   ├── scscf_node.*        S-CSCF + MTAS: registrar, call control, services
│   │   ├── ims_hss.*           IMS-HSS: Diameter Cx (SAR/SAA)
│   │   ├── ims_conferencing.h  3-party conference logic
│   │   └── ims_diagrams.h      ASCII call-flow diagrams for logs
│   ├── ims_main.cpp          Single-process demo (mme_ims)
│   ├── ims_server_main.cpp   Multi-terminal server (ims_server)
│   └── ue_sim.cpp             Multi-terminal UE client (ue_sim)
├── docs/
│   ├── BEGINNER_IMS.md        No-jargon "how does a phone call work" guide
│   ├── ENGINEER_IMS.md        Call flows, IEs, interview Q&A
│   ├── IMS_COMPLETE_GUIDE.md  All nodes, flows, IEs (Ericsson MTAS prep)
│   └── VOLTE_IMS_INTERVIEW.md Senior technical-lead interview prep
├── merge_pcap.sh              Merge per-UE pcaps into one combined pcap
└── CMakeLists.txt
```

---

## Connection: 4G EPC ↔ IMS

```
mme_sim:  UE attaches → gets IP 10.0.0.1 (from P-GW)
                                    │
ims-sim:  UE uses 10.0.0.1 as SIP Contact → registers with IMS
          VoLTE INVITE → 200 OK → P-CSCF sends Rx to PCRF
          PCRF (same as mme_sim Phase 4!) creates QCI=1 bearer
          Voice flows on QCI=1, data on QCI=9 simultaneously
```

---

## Related 3GPP Standards

| Standard | What it covers |
|----------|---------------|
| TS 23.228 | IMS architecture |
| TS 29.229 | Diameter Cx (S-CSCF ↔ HSS) |
| TS 29.214 | Diameter Rx (P-CSCF ↔ PCRF) |
| RFC 3261  | SIP protocol |

> 4G EPC standards (TS 23.401, TS 29.274, TS 29.272, TS 29.212) are covered in
> [`../4g-simulator/`](../4g-simulator/).

---

## Author

Built as a deep-dive into telecom systems engineering — combining 8 years of production
experience in 4G/5G core networks with modern C++17 to create a fully educational,
open-source simulator.
