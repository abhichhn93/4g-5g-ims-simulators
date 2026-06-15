# 4G EPC Simulator in C++17

A production-grade simulation of a complete **4G LTE Core Network**,
built entirely from scratch in C++17 using raw TCP/UDP sockets, multithreading, and real
protocol message encoding.

> The companion **IMS/VoLTE** stack (P-CSCF/S-CSCF/MTAS/IMS-HSS, SIP REGISTER/INVITE/BYE)
> now lives in the sibling project [`../ims-simulator/`](../ims-simulator/) — kept
> separate so someone studying IMS isn't wading through 4G EPC code, and vice versa.
> A 5G core (gNB/AMF/UDM) lives in [`../5g-simulator/`](../5g-simulator/).

> **Why this exists:** The team of 10 engineers I worked with at Samsung R&D built similar
> systems. This simulator proves the same architecture can be understood, designed, and
> implemented solo — and serves as a hands-on learning platform for telecom + systems engineers.

---

## What It Implements

### `mme_sim` — 4G EPC (Evolved Packet Core)

```
[UE] ──S1AP──► [eNB] ──S1AP──► [MME] ──S6a──► [HSS]
                                  │
                              GTP-C/S11
                                  │
                               [S-GW] ──GTP-C/S5──► [P-GW] ──Gx──► [PCRF]
```

| Node | Port | Protocol | 3GPP Ref |
|------|------|----------|----------|
| eNB  | 36412 | S1AP (binary TLV) | TS 36.413 |
| MME  | 36412 | S1AP + NAS | TS 29.274 |
| HSS  | 3868  | Diameter S6a | TS 29.272 |
| S-GW | 2123  | GTP-Cv2 | TS 29.274 |
| P-GW | 2124  | GTP-Cv2 + Gx | TS 29.274 |
| PCRF | 3869  | Diameter Gx | TS 29.212 |

**Full 4G Attach Call Flow:**
```
Step 1  Attach Request         UE → eNB → MME
Step 2  Authentication         MME → HSS (AIR/AIA — RAND/XRES/AUTN/KASME)
Step 3  NAS Security Setup     MME → UE  (Security Mode Command/Complete)
Step 4  Session Creation       MME → S-GW → P-GW (Create Session Request/Response)
Step 5  PCRF Policy            P-GW → PCRF (Credit Control Request — QCI assignment)
Step 6  Bearer Setup           MME → eNB → UE (S1AP E-RABSetup)
Step 7  Attach Complete        UE → eNB → MME
Step 8  UE gets IP address     10.0.0.x (assigned by P-GW)
```

---

## Color-Coded Live Output

Every node prints in its own color — watch packets flow in real time:

| Color | Node |
|-------|------|
| 🟢 Bold Green | eNB |
| 🔵 Bold Blue | MME |
| 🟡 Bold Yellow | HSS |
| 🩵 Bold Cyan | PCRF |
| 🟣 Bold Magenta | S-GW |
| 🟠 Orange | P-GW |

---

## Build & Run

### Prerequisites

**macOS:**
```bash
xcode-select --install          # installs clang + cmake
brew install cmake              # if not already installed
```

**Ubuntu/Debian Linux:**
```bash
sudo apt update
sudo apt install -y build-essential cmake g++
```

**Windows (WSL2 recommended):**
```bash
wsl --install                   # enable WSL2
# then follow Ubuntu steps above inside WSL
```

### Build

```bash
git clone https://github.com/YOUR_USERNAME/mme-simulator.git
cd mme-simulator
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

This produces `build/mme_sim` — the 4G EPC simulator.

### Run 4G EPC Simulator

```bash
cd build
./mme_sim
```

**Commands:**
```
CR 1        → Trigger 4G Attach for UE-1 (full 8-step call flow)
CR 5        → Bulk attach — 5 UEs simultaneously  
STATUS      → Show all attached UEs and bearer state
DELETE 1    → Detach UE-1 (release bearer)
QUIT        → Shutdown all nodes
```

---

## Capture Packets in Wireshark

```bash
# Terminal 1 — capture all simulator traffic
sudo tcpdump -i lo0 \
  'port 36412 or port 3868 or port 2123 or port 2124 or port 3869' \
  -w ~/Desktop/mme_capture.pcap

# Terminal 2 — run simulator
./mme_sim   # type CR 1 to trigger attach
```

Open `mme_capture.pcap` in Wireshark. Use the custom Lua dissector:

```
Wireshark → Preferences → Protocols → Lua Scripts → Add:
  mme-simulator/mme_sim_dissector.lua
```

**Key Wireshark filters:**
```
tcp.port == 36412 and tcp.len > 0   # S1AP (eNB ↔ MME)
tcp.port == 3868  and tcp.len > 0   # Diameter S6a (MME ↔ HSS)
tcp.port == 2123  and tcp.len > 0   # GTP-C S11 (MME ↔ S-GW)
tcp.port == 3869  and tcp.len > 0   # Diameter Gx (P-GW ↔ PCRF)
```

---

## Project Structure

```
mme-simulator/
├── src/
│   ├── common/
│   │   ├── logger.h          Color-coded thread-safe logger
│   │   ├── socket_wrapper.h  RAII TCP socket (connect/accept/send/recv)
│   │   ├── tlv.h             Binary TLV message encoder/decoder
│   │   ├── message_types.h   All protocol message type enums
│   │   ├── thread_pool.h     Fixed thread pool (C++17 + condition_variable)
│   │   └── metrics.h         Attach/detach counters
│   ├── enb/                  eNB: S1AP server, NAS relay
│   ├── mme/                  MME: UE context, auth, bearer management
│   ├── hss/                  HSS: Diameter S6a, auth vector generation
│   ├── sgw/                  S-GW: GTP-C S11/S5 bearer
│   ├── pgw/                  P-GW: IP allocation, Gx to PCRF
│   ├── pcrf/                 PCRF: QCI policy, Gx Diameter
│   └── main.cpp              4G EPC main + UE CLI
├── docs/
│   ├── ARCHITECTURE.md       Detailed code walkthrough
│   ├── CALL_FLOWS.md         Step-by-step 3GPP call flows
│   ├── INTERVIEW_GUIDE.md    C++17, multithreading, design patterns Q&A
│   ├── SETUP.md              Platform-specific setup guide
│   └── WIRESHARK.md          Packet capture + analysis guide
├── mme_sim_dissector.lua     Wireshark Lua dissector
└── CMakeLists.txt
```

---

## C++ Concepts Demonstrated

| Concept | Where Used |
|---------|-----------|
| `std::thread` + `std::atomic` | Each node runs in its own thread; stop flag is atomic |
| `std::shared_mutex` + sharding | UE context store — concurrent read, exclusive write |
| `std::condition_variable` | Thread pool task queue; auth wait |
| RAII | `Socket` class — fd closed in destructor, no leaks |
| `std::unique_ptr` / `shared_ptr` | UE contexts, subscriber profiles |
| Flyweight pattern | Shared subscriber profile objects |
| Thread pool | Bulk UE attach handled by worker threads |
| Binary TLV serialization | All messages: Tag(2B) + Length(2B) + Value |
| State machine | UE: IDLE → AUTH → CONNECTED → BEARER_ACTIVE |

---

## Interview Topics Covered in Logs

Run `CR 1` and you'll see annotated logs for:
- **4G Attach** — every step with 3GPP TS references
- **EPS-AKA Authentication** — RAND, XRES, AUTN, KASME explained
- **GTP-C** — tunnel creation, TEID allocation
- **Diameter** — S6a AIR/AIA, Gx CCR/CCA
- **QCI** — policy assignment, dedicated bearer for VoLTE

---

## Related 3GPP Standards

| Standard | What it covers |
|----------|---------------|
| TS 23.401 | 4G EPC architecture |
| TS 29.274 | GTP-C (MME ↔ S-GW ↔ P-GW) |
| TS 29.272 | Diameter S6a (MME ↔ HSS) |
| TS 29.212 | Diameter Gx (P-GW ↔ PCRF) |

> IMS standards (TS 23.228, TS 29.229, TS 29.214, RFC 3261) are covered in
> [`../ims-simulator/`](../ims-simulator/).

---

## Author

Built as a deep-dive into telecom systems engineering — combining 8 years of production
experience in 4G/5G core networks with modern C++17 to create a fully educational,
open-source simulator.

Connect on [LinkedIn](https://www.linkedin.com/in/YOUR_PROFILE)
