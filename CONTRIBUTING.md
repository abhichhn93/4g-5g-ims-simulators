# Contributing

## Quick Start

```bash
git clone https://github.com/abhichhn93/4g-5g-ims-simulators.git
cd 4g-5g-ims-simulators
```

### Build 4G Simulator
```bash
cd 4g-simulator
cmake -B build && cmake --build build -j4
./build/mme_sim
```

### Build IMS Simulator
```bash
cd ims-simulator
cmake -B build && cmake --build build -j4
./build/mme_ims
```

### Build 5G Simulator
```bash
cd 5g-simulator
cmake -B build && cmake --build build -j4
# Terminal 1: ./build/nrf_sim
# Terminal 2: ./build/udm_sim
# Terminal 3: ./build/amf_sim
# Terminal 4: ./build/gnb_sim
```

### Run with Docker Compose
```bash
# 4G full stack
cd 4g-simulator && docker compose up

# 5G full stack
cd 5g-simulator && docker compose up

# IMS full stack
cd ims-simulator && docker compose up
```

## Requirements

- C++17 compiler (GCC 10+ or Clang 12+)
- CMake 3.16+
- Docker + Docker Compose (optional)
- Wireshark (to inspect generated pcap files)

## Log Modes

All simulators support three verbosity levels:

```
MODE BEGINNER   # plain English narration, good for newcomers
MODE ENGINEER   # protocol fields, IEs, 3GPP spec references
MODE INTERVIEW  # Q&A format — "Why does MME send AIR to HSS?"
```

## PR Guidelines

- One feature or bug fix per PR
- Keep BEGINNER/ENGINEER/INTERVIEW log narration in all new flows
- Every new protocol message must be written to the pcap file
- Add at least one entry to the relevant `docs/INTERVIEW_QA.md`
- Update `STATE_OF_THE_UNION.md` with what changed

## Coding Standards

- C++17 only; no external libraries (OpenSSL allowed for crypto)
- RAII for all sockets and file handles (`socket_wrapper.h` pattern)
- No raw `new`/`delete` — use `std::unique_ptr` / `std::shared_ptr`
- No magic numbers — use named constants or enums

## File Structure

```
4g-simulator/   — eNB, MME, HSS, S-GW, P-GW, PCRF (S1AP/GTPv2/Diameter)
ims-simulator/  — P-CSCF, S-CSCF, MTAS, IMS-HSS  (SIP/SDP/Diameter Cx/Rx)
5g-simulator/   — gNB, AMF, UDM, NRF, SMF, UPF    (HTTP SBI / PFCP)
```
