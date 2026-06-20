# IMS / VoLTE Simulator in C++17

[![IMS Simulator Build](https://github.com/abhichhn93/4g-5g-ims-simulators/actions/workflows/ims-build.yml/badge.svg)](https://github.com/abhichhn93/4g-5g-ims-simulators/actions/workflows/ims-build.yml)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](../LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://en.cppreference.com/w/cpp/17)

A from-scratch simulation of the **IMS (IP Multimedia Subsystem)** — the SIP +
Diameter network that carries VoLTE voice calls — built in C++17 using raw
TCP sockets, multithreading, and real protocol message encoding (SIP text
messages, Diameter-style TLV).

> Sibling project to [`../4g-simulator/`](../4g-simulator/) (4G EPC) and
> [`../5g-simulator/`](../5g-simulator/) (5G core). A UE attaches to 4G EPC
> first to get an IP address, then uses that IP to register with IMS here.

---

## What It Implements

```
[UE-A]──SIP:5060──►[P-CSCF]──SIP:5060──►[I-CSCF]──Cx:3870──►[IMS-HSS]
                      │                      │
                      │                  UAR/UAA
                      │                      │
                   Rx(Dia)           ────[S-CSCF]──Cx SAR/SAA──►[IMS-HSS]
                      │                      │ISC
                   PCRF                  [MTAS] (Ericsson AS)
                      │                      │Mr (SIP)
                   P-GW              [MRFC]──Cr:2944──►[MRFP]
                   QCI=1
```

| Node | Port | Interface | What it does |
|------|------|-----------|-------------|
| P-CSCF | 5060 | Gm (UE), Mw (I/S-CSCF), Rx (PCRF) | First SIP contact; Rx→PCRF for QCI=1 bearer |
| I-CSCF | 5060 | Cx (UAR/UAA→HSS) | Queries HSS to find which S-CSCF serves the user |
| S-CSCF | 5070 | Mw, ISC (MTAS) | SIP registrar; applies iFC; invokes MTAS |
| MTAS   | 5070 | ISC (S-CSCF), Mr (MRFC) | Service logic: OIP/OIR, barring, CW, conf, CDR |
| IMS-HSS| 3870 | Cx (SAR/SAA) | Subscriber profile + iFC (Initial Filter Criteria) |
| MRFC   | 5060/2944 | Mr (MTAS), Cr (MRFP) | Conference state machine; controls MRFP via H.248 |
| MRFP   | 2944 (H.248) | Cr (MRFC) | DSP — actual 3-party audio mixing |

---

## Build

```bash
cd ims-simulator
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Produces: **`mme_ims`** (single-process demo), **`ims_server`** (multi-terminal server), **`ue_sim`** (UE client).

---

## Quick Start — One-Shot Demo Scripts

The fastest way to see the full IMS flow with all logs and pcap files generated automatically.

### 2-Party Call (REG → CALL → HOLD → RESUME → BYE)

```bash
cd ims-simulator          # from repo root
./demo_2party_call.sh     # ENGINEER mode (raw IEs, 3GPP references)
```

Or in plain-English mode:

```bash
./demo_2party_call.sh BEGINNER
```

**What it does automatically:**
1. Starts `ims_server` (P-CSCF + S-CSCF + IMS-HSS)
2. Starts `ue_sim A` (caller) and `ue_sim B` (callee)
3. Runs: `REG` → `REG` → `CALL B` → `ACCEPT` → `HOLD` → `RESUME` → `BYE` → `QUIT`
4. Merges all pcaps and prints server log tail

**Output files** (all in `build/`):

| File | What it contains |
|------|-----------------|
| `ims_server_demo.log` | ~40 KB — full server log: P-CSCF + S-CSCF + MTAS output with 3GPP IE detail |
| `ims_A_demo.log` | ~5 KB — UE-A terminal: what the caller sees step by step |
| `ims_B_demo.log` | ~5 KB — UE-B terminal: what the callee sees step by step |
| `ims_server_capture.pcap` | Server-side pcap (all SIP + Diameter) |
| `ims_combined.pcap` | Merged from all 3 UE pcaps — **open this in Wireshark** |

---

### Full Demo (3 UEs: 2-party call + conference + HOLD/RESUME)

```bash
./demo_full_call.sh          # ENGINEER mode
./demo_full_call.sh BEGINNER # plain-English mode
```

Adds UE-C and runs: `REG×3` → `CALL B` → `ACCEPT` → `CONF C` → `ACCEPT` (UE-C) → `HOLD` → `RESUME` → `BYE`

Additional output: `ims_C_demo.log` (UE-C terminal).

---

## Web Visualizer — Animated Call Flow Diagram

**Open `volte_call_flow.html` directly in any browser** — no server needed.

```bash
open ims-simulator/volte_call_flow.html   # macOS
xdg-open ims-simulator/volte_call_flow.html  # Linux
```

Shows the **complete VoLTE flow** as an animated sequence diagram with all nodes, IPs, ports, and IE details:

| Phase | Steps | What you see |
|-------|-------|-------------|
| 1 — LTE Attach | 15 | UE→eNB→MME, S6a AIR/AIA (RAND/XRES/AUTN/KASME), GTP-C S11/S5, Gx CCR/CCA, E-RABSetup |
| 2 — IMS PDN Setup | 3 | PDN Connectivity Req (APN=ims), QCI=5 bearer, P-CSCF address via PCO |
| 3 — IMS Registration | 10 | REGISTER→P-CSCF→**I-CSCF**→Cx UAR/UAA→S-CSCF→Cx SAR/SAA→MTAS 3rd-party REG→200 OK |
| 4 — VoLTE Call | 18 | INVITE→iFC→MTAS checks→183+PRACK→Rx AAR→QCI=1 E-RABSetup→UPDATE preconditions→180→200→ACK→RTP |
| 5 — Teardown | 6 | BYE→CDR close→Rx STR→Gx RAR→Delete Bearer |
| 6 — Conference | 18 | REFER→202→NOTIFY→**MRFC H.248 CreateContext**→MRFP mixing→UE-C INVITE→SUBSCRIBE→NOTIFY XML |

**Controls:**

| Action | How |
|--------|-----|
| Animate step by step | Click **▶ Play** or press `Space` |
| Advance one message | Click **⏭ Step** or press `→` |
| Reset | Click **⏮ Reset** or press `R` |
| Show all at once | **Double-click** the diagram |
| Filter to one phase | Click phase tabs (LTE / IMS PDN / IMS Reg / VoLTE Call / Teardown / Conference) |
| See IE details | **Click any message arrow** → right panel shows every header with name, value, and why it matters |
| Key IEs | Rows highlighted in **orange** with ★ = most interview-critical fields |
| Speed | Dropdown top-right: Slow / Normal / Fast / Very Fast |

**Nodes shown** (left → right): `UE-A → eNB → MME → HSS → S-GW → P-GW → PCRF → P-CSCF → I-CSCF → S-CSCF → MTAS → IMS-HSS → MRFC → MRFP → UE-B → UE-C`

---

## Manual Run — Multi-Terminal Mode

```bash
# Terminal 1 — start IMS server FIRST
cd build
./ims_server
# or: LOG_LEVEL=BEGINNER ./ims_server

# Terminal 2 — UE-A (caller)
cd build && ./ue_sim A

# Terminal 3 — UE-B (callee)
cd build && ./ue_sim B

# Terminal 4 — UE-C (for conference)
cd build && ./ue_sim C
```

### LOG_LEVEL (set on the server terminal only)

| Value | Output |
|-------|--------|
| `ENGINEER` (default) | Raw SIP IEs, Diameter AVPs, hop-by-hop header diffs, 3GPP TS references |
| `BEGINNER` | Plain-English narration ("UE-B's phone is ringing") — no protocol jargon |

### Server commands

| Command | What it does |
|---------|-------------|
| `STATUS` | Show all registered UEs (IMPU, Contact IP, call state) |
| `BARR A\|B\|C` | Enable MTAS call barring (BAOC) — next CALL to this UE gets 603 Decline |
| `UNBARR A\|B\|C` | Lift call barring |
| `QUIT` | Shut down cleanly, finalize pcap files |

### UE commands (type in each UE terminal)

| Command | What happens |
|---------|-------------|
| `REG` | SIP REGISTER → P-CSCF → I-CSCF → S-CSCF → Cx SAR/SAA → 200 OK |
| `CALL B` | SIP INVITE to UE-B → 100 Trying → 183 Session Progress → PRACK → 180 Ringing |
| `ACCEPT` | Send 200 OK (SDP answer, AMR-WB/16000) → ACK → call established |
| `REJECT` | Send 486 Busy Here |
| `HOLD` | re-INVITE with `a=sendonly` (you stop sending, callee hears hold music) |
| `RESUME` | re-INVITE with `a=sendrecv` (bidirectional voice restored) |
| `VIDEO` | re-INVITE adding `m=video H264/90000` (QCI=2 bearer added) |
| `VOICE` | re-INVITE with `m=video port=0` (drop video, voice only) |
| `CONF C` | REFER → 202 Accepted → MRFC setup → INVITE UE-C → conference active |
| `BYE` | End call (Rx STR → PCRF → QCI=1 bearer released) |
| `STATUS` | Show this UE's state (IMPU, IP, call ID, codec) |
| `QUIT` | Disconnect from server, finalize pcap |

### Step-by-step demo flows

**2-party call:**
```
UE-A: REG        → wait for ✓ IMS REGISTRATION COMPLETE
UE-B: REG        → wait for ✓ IMS REGISTRATION COMPLETE
UE-A: CALL B     → UE-B shows 📞 INCOMING CALL
UE-B: ACCEPT     → both UEs show call connected
UE-A: HOLD       → UE-B shows ⏸ PUT ON HOLD
UE-A: RESUME     → both UEs show ▶ CALL RESUMED
UE-B: BYE        → both UEs show ✓ CALL ENDED
```

**Call barring:**
```
Server: BARR B
UE-A: CALL B     → UE-A gets 603 Decline (MTAS BAOC active)
Server: UNBARR B
UE-A: CALL B     → works normally
```

**Call waiting:**
```
(A and B in a call)
UE-C: REG
UE-C: CALL B     → UE-B shows second 📞 INCOMING CALL (call waiting)
```

**3-party conference:**
```
UE-A: REG    UE-B: REG    UE-C: REG
UE-A: CALL B → UE-B: ACCEPT
UE-A: CONF C → UE-C shows 👥 CONFERENCE INVITE → UE-C: ACCEPT
  Server shows: REFER → 202 Accepted → MRFC H.248 CreateContext
               → NOTIFY (trying → early → terminated)
               → SUBSCRIBE + NOTIFY (conference-info+xml)
All 3 UEs now in conference (MRFP mixing 3 RTP streams)
```

---

## What's in the PCAP

After any run, `build/ims_combined.pcap` contains the following complete SIP + Diameter flow:

```
REGISTRATION (per UE)
  REGISTER → 200 OK REGISTER
  Diameter Cx SAR/SAA (subscriber profile + iFC)

CALL SETUP
  INVITE → 100 Trying → 183 Session Progress + SDP (QoS preconditions)
         → PRACK → 200 OK (PRACK)
         → 180 Ringing → 200 OK INVITE + SDP answer → ACK
  Diameter Rx AAR/AAA (P-CSCF → PCRF, QCI=1 bearer)

MID-CALL
  re-INVITE (CSeq:2, a=sendonly) → 200 OK          ← HOLD
  re-INVITE (CSeq:3, a=sendrecv) → 200 OK          ← RESUME
  re-INVITE (CSeq:4, m=video)    → 200 OK          ← VIDEO
  re-INVITE (CSeq:5, m=video 0)  → 200 OK          ← VOICE

CONFERENCE
  REFER (Refer-To: UE-C) → 202 Accepted
  NOTIFY (trying)  body: SIP/2.0 100 Trying
  SUBSCRIBE (Event: conference) → 200 OK
  INVITE (Call-ID=-conf) → 100 Trying → 183 → PRACK → 200 OK PRACK
  NOTIFY (early)   body: SIP/2.0 180 Ringing
  180 Ringing → 200 OK INVITE → ACK
  NOTIFY (terminated)  body: SIP/2.0 200 OK
  NOTIFY (conference-info+xml: UE-A/B/C all connected)

TEARDOWN
  BYE → 200 OK BYE
  Diameter Cx SAR (de-registration on QUIT)
```

### Wireshark filters

Open `build/ims_combined.pcap` in Wireshark and use these filters:

**By message type:**
```
sip                                  → all SIP messages
sip.Method == "REGISTER"             → registration only
sip.Method == "INVITE"               → initial INVITE + re-INVITEs + conference leg
sip.Method == "PRACK"                → reliable provisional ACK (VoLTE mandatory 100rel)
sip.Method == "REFER"                → conference trigger
sip.Method == "NOTIFY"               → REFER status + conference-state XML updates
sip.Method == "SUBSCRIBE"            → conference-state subscription
sip.Method == "BYE"                  → call teardown
sip.Status-Code == 100               → 100 Trying
sip.Status-Code == 180               → 180 Ringing (callee alerting — dialog established)
sip.Status-Code == 183               → 183 Session Progress (QoS preconditions + early SDP)
sip.Status-Code == 200               → all 200 OK (REG / PRACK / INVITE / BYE)
sip.Status-Code == 202               → 202 Accepted (REFER response — async, NOT 200 OK)
diameter                             → Cx SAR/SAA + Rx AAR/AAA
```

**By call leg (Call-ID):**
```
sip.Call-ID contains ":B-2"          → A↔B main call
sip.Call-ID contains "-conf"         → conference INVITE leg to UE-C
sip.Call-ID contains "-sub"          → SUBSCRIBE/NOTIFY (conference-state)
```

**By node (IP address):**
```
ip.addr == 10.0.0.1    → UE-A (caller)
ip.addr == 10.0.0.2    → UE-B (callee)
ip.addr == 10.0.0.3    → UE-C (conference invitee)
ip.addr == 10.0.0.8    → P-CSCF (SIP proxy, first hop)
ip.addr == 10.0.0.9    → S-CSCF (SIP registrar + MTAS)
ip.addr == 10.0.0.11   → MRFC (conference bridge controller)
```

**Conference messages only:**
```
sip.Method == "REFER" or sip.Method == "NOTIFY" or sip.Status-Code == 202
  or sip.Call-ID contains "-conf" or sip.Call-ID contains "-sub"
```

### PCAP IP→node mapping

| IP | Node | IMS constant |
|----|------|-------------|
| 10.0.0.1 | UE-A | `IP_UE` |
| 10.0.0.2 | UE-B | `IP_UE_B` |
| 10.0.0.3 | UE-C | `IP_UE_C` |
| 10.0.0.8 | P-CSCF | `IP_PCSCF` |
| 10.0.0.9 | S-CSCF | `IP_SCSCF` |
| 10.0.0.10 | MTAS | `IP_MTAS` |
| 10.0.0.11 | MRFC (conference bridge) | `IP_MRFC` |

---

## Single-Process Demo (`mme_ims`)

For a quick combined pcap without needing multiple terminals:

```bash
cd build
./mme_ims
```

**Commands:**
```
REG A|B|C|ALL  → IMS registration (REGISTER → Cx SAR/SAA → 200 OK)
CALL A B       → VoLTE call A → B (INVITE → MTAS → 200 OK → Rx AAR → QCI=1)
CONF           → 3-party conference (A+B+C via MRFC/MRFP)
WAIT           → Call waiting scenario
BARR           → Call barring demo (603 Decline)
BYE            → End call (Rx STR → PCRF → QCI=1 released)
STATUS         → Show registered UEs
QUIT           → Shutdown (saves ims_capture.pcap)
```

---

## Project Structure

```
ims-simulator/
├── src/
│   ├── common/
│   │   ├── logger.h            Color-coded thread-safe logger (LOG_LEVEL env var)
│   │   ├── visual_logger.h     Step-by-step call-flow banners (VLog::step)
│   │   ├── socket_wrapper.h    RAII TCP socket (connect/accept/send/recv)
│   │   ├── tlv.h               Binary TLV message encoder/decoder
│   │   ├── message_types.h     All protocol message type enums
│   │   ├── exceptions.h        Custom exception types
│   │   └── pcap_writer.*       Pcap capture writer (synthesizes real SIP/Diameter packets)
│   ├── ims/
│   │   ├── sip.h               SIP message types + TLV encoding
│   │   ├── sip_text.h          Real SIP/SDP text builders (REGISTER/INVITE/183/PRACK/
│   │   │                         ACK/BYE/200/REFER/NOTIFY/SUBSCRIBE/conference-info+xml)
│   │   ├── mtas_state.h        MTAS call barring state (thread-safe singleton)
│   │   ├── pcscf_node.*        P-CSCF: SIP proxy, Rx interface, call routing
│   │   ├── scscf_node.*        S-CSCF + MTAS: registrar, iFC triggers, re-INVITE,
│   │   │                         conference (REFER/NOTIFY/MRFC/H.248 pcap)
│   │   ├── ims_hss.*           IMS-HSS: Diameter Cx (SAR/SAA, subscriber profile + iFC)
│   │   ├── ims_conferencing.h  Conference logic helpers
│   │   └── ims_diagrams.h      ASCII call-flow diagrams for terminal logs
│   ├── ims_main.cpp            Single-process demo binary (mme_ims)
│   ├── ims_server_main.cpp     Multi-terminal server binary (ims_server)
│   └── ue_sim.cpp              Multi-terminal UE client binary (ue_sim)
├── docs/
│   ├── BEGINNER_IMS.md         No-jargon "how does a phone call work" guide
│   ├── ENGINEER_IMS.md         Full IE tables, call flows, interview Q&A
│   ├── IMS_COMPLETE_GUIDE.md   All nodes, all flows, all IEs (Ericsson MTAS prep)
│   └── VOLTE_IMS_INTERVIEW.md  Senior tech-lead interview prep (12 key Q&As)
├── volte_call_flow.html        ★ Animated web visualizer — open in any browser
├── demo_2party_call.sh         One-shot 2-party call demo (auto log + pcap)
├── demo_full_call.sh           One-shot full demo: 3 UEs + conference + HOLD/RESUME
├── merge_pcap.sh               Merge per-UE pcaps → ims_combined.pcap
└── CMakeLists.txt
```

---

## Connection: 4G EPC ↔ IMS

```
4G Attach (4g-simulator):
  CR 1 → UE attaches → gets IP 10.0.0.1 from P-GW (QCI=9 default bearer)
                                    │
IMS PDN Setup:                      │ Same IP used here ↓
  PDN Connectivity Req (APN=ims) → QCI=5 signaling bearer
  → P-CSCF address returned in PCO → UE now knows where to send SIP

IMS Registration:
  REGISTER (Contact: sip:ue@10.0.0.1:5060) ← 4G IP in SIP Contact!
  → P-CSCF → I-CSCF → S-CSCF → MTAS

VoLTE Call:
  INVITE → SDP negotiation → P-CSCF sends Rx AAR → PCRF
  → Gx RAR → P-GW creates QCI=1 dedicated bearer
  → Voice flows on QCI=1 (guaranteed 64kbps) alongside data on QCI=9
```

---

## Interview Reference — Key Concepts

| Concept | What to say |
|---------|-------------|
| **iFC** | Initial Filter Criteria — stored in HSS, delivered via Cx SAA. Tells S-CSCF when to invoke MTAS (e.g. "on INVITE"). Key Ericsson concept. |
| **I-CSCF role** | Stateless query node — asks HSS (Cx UAR) which S-CSCF serves the user, forwards REGISTER there, then drops out of dialog. |
| **PRACK / 100rel** | VoLTE mandates Require:100rel. Provisional responses (183, 180) are acknowledged with PRACK (RAck header). Without it, caller doesn't know callee is ringing. |
| **QoS preconditions** | 183 carries SDP with `a=curr:qos none` + `a=des:qos mandatory`. Bearer MUST be created before alerting callee. UPDATE signals when bearer is ready. |
| **MRFC vs MRFP** | MRFC = controller (SIP, state machine). MRFP = DSP processor (H.248/Megaco, Cr interface). MRFC tells MRFP what to mix; MRFP does actual audio mixing. |
| **REFER → 202** | REFER always gets 202 Accepted (not 200 OK) because it's async. Progress via NOTIFY with sipfrag body. |
| **Conference-state** | UE subscribes (Event: conference) to MRFC. MRFC pushes conference-info+xml listing participants. New NOTIFY on join/leave. |
| **Rx AAR** | P-CSCF→PCRF after SDP negotiation. Carries codec, bandwidth, TFT filter. PCRF→P-GW creates QCI=1. This is the 4G↔IMS link. |

---

## Related 3GPP Standards

| Standard | What it covers |
|----------|---------------|
| TS 23.228 | IMS architecture |
| TS 24.229 | SIP/IMS procedures (REGISTER, INVITE, PRACK, UPDATE) |
| TS 29.229 | Diameter Cx (I-CSCF/S-CSCF ↔ HSS): UAR/UAA, SAR/SAA |
| TS 29.214 | Diameter Rx (P-CSCF ↔ PCRF): AAR/AAA, STR |
| TS 23.333 | MRFC procedures |
| TS 24.147 | Conference using IMS (REFER/NOTIFY/MRFC) |
| RFC 3261  | SIP protocol |
| RFC 3262  | PRACK / reliable provisional responses (100rel) |
| RFC 3311  | SIP UPDATE method |
| RFC 3312  | QoS preconditions (a=curr/a=des SDP attributes) |
| RFC 3515  | REFER method + NOTIFY for refer-state |
| RFC 3550  | RTP (Real-time Transport Protocol) |
| RFC 4575  | Conference event package (conference-info+xml) |
| RFC 3525  | H.248/Megaco (MRFC ↔ MRFP, Cr interface) |

> 4G EPC standards (TS 23.401, TS 29.274, TS 29.272, TS 29.212) are in [`../4g-simulator/`](../4g-simulator/).

---

## Author

Built as a deep-dive into telecom systems engineering — combining 8 years of production
experience in 4G/5G core networks with modern C++17 to create a fully educational,
open-source simulator.
