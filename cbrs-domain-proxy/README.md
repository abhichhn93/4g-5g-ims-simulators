# CBRS Domain Proxy — C++17 Simulator

A from-scratch simulation of the **CBRS (Citizens Broadband Radio Service)** control plane — implementing the WinnForum SAS-CBSD protocol (WINNF-TS-0016) in C++17 with raw TCP sockets and JSON messaging.

> Part of the [4G + IMS + 5G + CBRS simulator suite](../README.md) — built by a telecom engineer with 8 years of experience in 4G/5G core networks.

---

## What Is CBRS?

CBRS is the **3.5 GHz spectrum band (3550–3700 MHz)** in the US, shared dynamically between:

| Tier | Who | Priority |
|------|-----|----------|
| **Incumbent** | US Navy radar + satellite earth stations | Highest — must be protected |
| **PAL** (Priority Access License) | FCC-licensed operators (paid auction) | High — 10 MHz blocks |
| **GAA** (General Authorized Access) | Anyone with an approved CBSD | Lowest — opportunistic |

The key actors:
- **CBSD** (Citizens Broadband Radio Service Device) = the smart base station (small cell, macro cell, private 5G node)
- **Domain Proxy** = middleware that aggregates multiple CBSDs and manages their SAS communication
- **SAS** (Spectrum Access System) = cloud service (Google, Federated Wireless, CommScope) that coordinates spectrum use, protects incumbents via ESC sensors

---

## Architecture

```
                 WinnForum CBRS Protocol (WINNF-TS-0016)
                 JSON over TCP (real: HTTPS + mutual TLS)

  CBSD-1 ──TCP:8700──►                    ──TCP:8800──► SAS
  CBSD-2 ──TCP:8700──► Domain Proxy                     (Spectrum Access
  CBSD-3 ──TCP:8700──►    :8700                          System)
                        (this simulator)
```

**What each binary does:**

| Binary | Port | Role |
|--------|------|------|
| `sas_stub` | 8800 | Simulated SAS — grants spectrum, tracks CBSDs |
| `domain_proxy` | 8700 (←CBSDs) / 8800 (→SAS) | Aggregates CBSDs, enforces state machine |
| `cbsd_agent` | (connects to 8700) | Simulated CBSD with interactive CLI |

---

## Build

```bash
cd cbrs-domain-proxy
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Produces: `sas_stub`, `domain_proxy`, `cbsd_agent`

---

## Run

**3 terminals, start in this order:**

```bash
# Terminal 1 — SAS (start first)
cd build && ./sas_stub

# Terminal 2 — Domain Proxy
cd build && ./domain_proxy

# Terminal 3 — CBSD Agent (1=Cat-A 3555MHz, 2=Cat-A 3565MHz, 3=Cat-B 3580MHz)
cd build && ./cbsd_agent 1
```

**LOG_LEVEL** controls verbosity:

| Value | Output |
|-------|--------|
| `ENGINEER` (default) | JSON messages, state transitions, field details |
| `BEGINNER` | Plain-English story ("CBSD is now transmitting") |
| `INTERVIEW_T` | CBRS spec details, FCC rules, ESC sensors, PAL/GAA |
| `ALL` | Everything |

```bash
LOG_LEVEL=ALL ./sas_stub   # shows all log levels simultaneously
```

---

## CBSD Commands (type in `cbsd_agent` terminal)

| Command | What happens |
|---------|-------------|
| `REGISTER` | Sends RegistrationRequest (FCC ID, location, antenna info) → SAS assigns cbsdId |
| `GRANT` | Sends GrantRequest (desired freq + BW) → SAS assigns grantId + channel |
| `HEARTBEAT` | Sends HeartbeatRequest → refreshes transmit window → state = AUTHORIZED |
| `RELINQUISH` | Voluntarily returns spectrum grant → state = REGISTERED |
| `DEREGISTER` | Removes CBSD from SAS → state = UNREGISTERED |
| `STATUS` | Show current state, cbsdId, grant details |
| `QUIT` | Disconnect |

**Domain Proxy commands** (type in `domain_proxy` terminal):

| Command | What it does |
|---------|-------------|
| `STATUS` | Show all registered CBSDs and their states |
| `QUIT` | Shutdown |

---

## Complete Flow (what you see end-to-end)

```
Step 1 — REGISTER:
  CBSD → DP → SAS: {"type":"RegistrationRequest","fccId":"FCC-DEVICE-001",...}
  SAS → DP → CBSD: {"type":"RegistrationResponse","cbsdId":"CBSD-SAS-1","responseCode":0}
  State: UNREGISTERED → REGISTERED

Step 2 — GRANT:
  CBSD → DP → SAS: {"type":"GrantRequest","cbsdId":"CBSD-SAS-1","operationFrequencyMHz":3555,...}
  SAS → DP → CBSD: {"type":"GrantResponse","grantId":"GRANT-1","channelType":"GAA",...}
  State: REGISTERED → GRANTED

Step 3 — HEARTBEAT:
  CBSD → DP → SAS: {"type":"HeartbeatRequest","grantId":"GRANT-1","operationState":"GRANTED"}
  SAS → DP → CBSD: {"type":"HeartbeatResponse","heartbeatInterval":120,...}
  State: GRANTED → AUTHORIZED  ← CBSD is now transmitting on 3555 MHz

Step 4 — RELINQUISH:
  CBSD → DP → SAS: {"type":"RelinquishmentRequest","grantId":"GRANT-1"}
  State: AUTHORIZED → REGISTERED

Step 5 — DEREGISTER:
  CBSD → DP → SAS: {"type":"DeregistrationRequest","cbsdId":"CBSD-SAS-1"}
  State: REGISTERED → UNREGISTERED
```

---

## Spectrum Allocation State Machine

```
UNREGISTERED
    │  RegistrationRequest → RegistrationResponse (responseCode=0)
    ▼
REGISTERED      ← can request spectrum
    │  GrantRequest → GrantResponse (grantId assigned)
    ▼
GRANTED         ← has channel, not yet transmitting
    │  HeartbeatRequest → HeartbeatResponse (transmit window opens)
    ▼
AUTHORIZED      ← actively transmitting on granted channel
    │  RelinquishmentRequest → RelinquishmentResponse
    ▼
REGISTERED      ← spectrum freed, can request new grant
    │  DeregistrationRequest → DeregistrationResponse
    ▼
UNREGISTERED
```

The **Domain Proxy enforces** this state machine locally before forwarding to SAS — rejects out-of-order messages (e.g., GrantRequest before registration) without a cloud round-trip.

---

## Connection to 4G/5G

CBRS is the **spectrum layer** that private 5G networks (O-RAN, LTE-in-a-box) run on:

```
Private 5G on CBRS:
  UE (phone) ──NR Uu──► CBSD (gNB) ──N2/S1──► Core (AMF/MME)
                             │
                    (CBRS SAS protocol)
                             │
                         Domain Proxy
                             │
                            SAS (spectrum coordinator)

  The CBSD is simultaneously:
    - A 5G gNB serving UEs (like the gNB in this repo's 5g-simulator)
    - A CBRS client getting spectrum authorization from SAS
```

In a real private 5G deployment:
- The `gNB` in `5g-simulator/` is the radio function
- The `cbsd_agent` here manages that gNB's **right to transmit** on 3.5 GHz spectrum
- Without a valid SAS grant, the gNB cannot transmit (FCC Part 96 rule)

---

## SAS Response Codes (WinnForum WINNF-TS-0016 Table 14)

| Code | Meaning | When you see it |
|------|---------|----------------|
| 0 | SUCCESS | Normal operation |
| 100 | VERSION_MISMATCH | Wrong protocol version |
| 102 | MISSING_PARAM | Required field absent in request |
| 103 | INVALID_VALUE | Field value out of range |
| 105 | DEREGISTER | SAS-initiated deregistration (ESC event) |
| 300 | UNSUPPORTED_SPECTRUM | Requested channel unavailable |
| 302 | GRANT_CONFLICT | Another CBSD already has this channel |
| 400 | TERMINATED_GRANT | Grant revoked by SAS (missed heartbeat / ESC) |
| 401 | SUSPENDED_GRANT | Temporarily suspended (PAL priority) |

---

## Project Structure

```
cbrs-domain-proxy/
├── src/
│   ├── common/
│   │   ├── logger.h          Color-coded logger (LOG_LEVEL env var)
│   │   ├── socket_wrapper.h  RAII TCP socket (same pattern as 4G/5G sims)
│   │   └── wire.h            JSON framing + flat JSON builder/parser
│   ├── proxy/
│   │   ├── spectrum_state.h  State machine enum + WinnForum response codes
│   │   ├── cbsd_registry.h   Thread-safe CBSD store (mutex + unordered_map)
│   │   └── domain_proxy.cpp  ★ Domain Proxy main (accept loop, SAS forward)
│   ├── sas/
│   │   └── sas_stub.cpp      ★ Simulated SAS (grants, heartbeats, deregistration)
│   └── cbsd/
│       └── cbsd_agent.cpp    ★ CBSD simulator CLI (all 5 protocol steps)
├── docs/
│   └── CBRS_INTERVIEW_GUIDE.md  Interview prep: CBRS concepts + Q&A
└── CMakeLists.txt
```

---

## Related Standards

| Standard | What it covers |
|----------|---------------|
| WINNF-TS-0016 | WinnForum SAS-CBSD Interface Specification (the protocol this sim implements) |
| FCC Part 96 | CBRS rules: power limits, CBSD categories, SAS certification requirements |
| FCC Part 96.41 | EIRP limits: Cat-A ≤30 dBm, Cat-B ≤47 dBm |
| 3GPP TS 38.104 | NR operating bands — Band n48 = 3550-3700 MHz (CBRS band) |
| O-RAN WG4 | Fronthaul interface between O-RU (radio) and O-DU (baseband) |
