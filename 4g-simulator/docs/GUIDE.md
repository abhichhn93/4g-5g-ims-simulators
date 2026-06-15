# 4G EPC MME Simulator — Complete Guide

> **For interview prep, GitHub readers, and Wireshark analysis**
> This document covers: architecture, full attach call flow, every IE explained,
> how to find each packet in Wireshark, threading model, and interview Q&A.

---

## 1. What This Project Is

A working **4G EPC (Evolved Packet Core) simulator** written in C++17.
Every node runs as a real thread. Nodes talk over real TCP/UDP sockets on localhost.
You can capture every packet in Wireshark and see the actual protocol bytes.

**Why build this?**
- Understand 4G attach procedure end-to-end by *seeing* it happen
- Learn C++ multithreading patterns in a real-world context
- Interview preparation: explain any step of the attach flow with confidence

---

## 2. Nodes and Ports

```
┌──────────────────────────────────────────────────────────────────┐
│                         YOUR LAPTOP                              │
│                                                                  │
│  [eNB]──TCP:38412──[MME]──TCP:3868──[HSS]                       │
│                      │                                           │
│                      └──UDP:2123──[S-GW]──UDP:2124──[P-GW]      │
│                                              │                   │
│                                              └──TCP:3869──[PCRF] │
│                                                                  │
│  MME also binds UDP:2125 (its own S11 port to receive from S-GW) │
└──────────────────────────────────────────────────────────────────┘
```

| Node | Thread | Protocol | Port | Real 4G port |
|------|--------|----------|------|-------------|
| eNB  | enb_th | TCP (S1AP) | 38412 | SCTP 36412 |
| MME  | mme_th | TCP + UDP | 38412 + 2125 | — |
| HSS  | hss_th | TCP (Diameter S6a) | 3868 | SCTP 3868 |
| S-GW | sgw_th | UDP (GTP-C S11) | 2123 | UDP 2123 |
| P-GW | pgw_th | UDP (GTP-C S5) | 2124 | UDP 2124 |
| PCRF | pcrf_th | TCP (Diameter Gx) | 3869 | SCTP 3868 |

---

## 3. How to Build and Run

```bash
cd 4g-simulator
mkdir -p build && cd build
cmake .. && make -j4
./mme_sim
```

**Commands inside the simulator:**
```
CR 1        → attach 1 UE (full 14-step flow)
CR 5        → attach 5 UEs sequentially
BULK 5      → attach 5 UEs via thread pool (prints P95/P99 metrics)
STATUS      → show all UE contexts (IMSI, IP, EMM state)
QUIT        → clean shutdown (Ctrl+C also works)
```

**Capture packets while running:**
```bash
# Terminal 1 — capture before you run the simulator
sudo tcpdump -i lo0 '(port 38412 or port 3868 or port 2123 or port 2124 or port 3869)' \
  -w ~/Desktop/mme_capture.pcap

# Terminal 2 — run simulator
./mme_sim
# type: CR 1
# type: QUIT

# Then open mme_capture.pcap in Wireshark
```

---

## 4. Full 4G Attach Call Flow (what happens when you type `CR 1`)

```
USER TYPES: CR 1
     │
     ▼
[eNB] builds S1AP InitialUEMessage
  ↓ TCP:38412
[MME] creates UE context
  ↓ TCP:3868
[HSS] generates auth vectors (RAND, AUTN, XRES, Kasme)
  ↓ TCP:3868 (back to MME)
[MME] sends Auth Request to UE via eNB
  ↓ TCP:38412
[eNB] simulates UE computing RES, sends Auth Response
  ↓ TCP:38412 (back to MME)
[MME] validates RES == XRES → auth OK
  ↓ UDP:2123
[S-GW] allocates S11 TEID + S1-U TEID
  ↓ UDP:2124
[P-GW] sends CCR-I to PCRF → gets CCA-I → allocates UE IP (10.0.0.x)
  ↓ UDP:2124 (back to S-GW)
[S-GW] sends Create Session Response to MME
  ↓ UDP:2123 (back to MME)
[MME] sends Initial Context Setup Request to eNB (with Attach Accept + bearer info)
  ↓ TCP:38412
[eNB] allocates eNB S1-U TEID, sends Initial Context Setup Response
  ↓ TCP:38412 (back to MME)
[MME] sends Modify Bearer Request to S-GW (tells S-GW the eNB's TEID)
  ↓ UDP:2123
[S-GW] acknowledges → data path is live
[eNB] simulates UE sending Attach Complete
  ↓ TCP:38412
[MME] sets EMM state = REGISTERED ✓
     UE has IP address and LTE data connectivity
```

---

## 5. Step-by-Step: Every Message, Every IE

### Step 1 — eNB → MME: S1AP InitialUEMessage
**Spec: TS 36.413 §9.1.7.1**
**Interface: S1-MME | Our port: TCP 38412 | Real: SCTP 36412**

| IE | Value | Why it matters |
|----|-------|----------------|
| eNB-UE-S1AP-ID | 1, 2, 3... | eNB's handle for this UE. MME must store this and echo it back in every future message TO the eNB for this UE |
| NAS-PDU | opaque bytes | Contains Attach Request. eNB NEVER reads NAS — it's encrypted end-to-end between UE and MME. eNB just wraps and forwards |
| TAI (MCC=404, MNC=10, TAC=1) | India operator | Tells MME which tracking area. MME uses it to build the TAI List in Attach Accept and to select which S-GW to use |
| E-UTRAN CGI (Cell ID=1) | exact cell | For lawful intercept logging and location services |
| RRC-Establishment-Cause | 3 = mo-Signalling | Why UE is connecting. During network overload, MME rejects mo-Data (4) first but still processes mo-Signalling (3) |

**Inside NAS-PDU — Attach Request (TS 24.301 §8.2.4):**

| NAS IE | Value | Why it matters |
|--------|-------|----------------|
| Protocol Discriminator | 0x07 | 07 = EPS Mobility Management (EMM). Distinguishes NAS layers |
| Security Header | 0x00 | Plain NAS — no encryption yet. **IMSI is in cleartext! This is the IMSI catcher vulnerability.** 5G fixes this with SUCI |
| Message Type | 0x41 | 0x41 = Attach Request |
| EPS Attach Type | 1 | 1 = EPS-only (data). 2 = combined EPS+IMSI (for CSFB voice via MSC) |
| NAS KSI | 7 | 7 = no cached security context → MME MUST run full AKA authentication. If 0-6, MME can skip auth (fast re-attach) |
| Identity Type | 1 | 1 = IMSI. On subsequent attaches: 6 = GUTI (temporary ID to hide IMSI) |
| IMSI | 404100000000001 | 15-digit subscriber ID. MCC=404 (India), MNC=10, MSIN=00000000001 |
| UE Network Capability | 0xE0 | Bitmask: EEA0/1/2 (encryption algorithms) + EIA1/2 (integrity algorithms) supported |

---

### Step 2 — MME → HSS: Diameter AIR
**Spec: TS 29.272 §7.2.5**
**Interface: S6a | Our port: TCP 3868 | Real: SCTP 3868, Diameter App-ID=16777251**

| IE | Value | Why |
|----|-------|-----|
| IMSI | 404100000000001 | HSS looks up this subscriber's secret key (Ki) in its database |
| Visited-PLMN | MCC+MNC | Roaming check: is this UE allowed to attach to this network? |

---

### Step 3 — HSS → MME: Diameter AIA
**Spec: TS 29.272 §7.2.6**

| IE | Value | Why |
|----|-------|-----|
| RAND | 16 random bytes | Random challenge sent to UE. Both UE and HSS use this as input to Milenage algorithm with Ki |
| AUTN | 16 bytes | Authentication Token. UE verifies this to authenticate the NETWORK (mutual auth). Contains: SQN ⊕ AK \| AMF \| MAC |
| XRES | 8 bytes | Expected Response. MME stores this. Will compare with what UE sends back |
| Kasme | 32 bytes | Base key. MME derives NAS encryption key (Knas_enc) and integrity key (Knas_int) from this |

**What is AKA? (Authentication and Key Agreement)**
```
HSS side:                          UE side (USIM chip):
Ki (secret key, never leaves HSS)  Ki (same secret key, in SIM card)
+ RAND (random)                    + RAND (received from MME)
+ SQN (sequence number)            
↓ Milenage algorithm               ↓ same Milenage algorithm
→ AUTN, XRES, Kasme               → verify AUTN (prove network is real)
                                   → compute RES (send to MME)
                                   → compute Kasme (derive NAS keys)

MME checks: RES == XRES → authenticated!
```

**INTERVIEW Q: "What is mutual authentication in 4G?"**
UE verifies AUTN (proves network has the right Ki) before responding.
Network verifies RES == XRES (proves UE has the right Ki).
Neither side ever sends Ki — it's a zero-knowledge proof using Milenage.

---

### Step 4 — MME → eNB: DL NAS Transport (Auth Request)
**Spec: TS 36.413 §9.1.7.2**

MME wraps the NAS Authentication Request inside an S1AP Downlink NAS Transport.

| IE | Value | Why |
|----|-------|-----|
| MME-UE-S1AP-ID | 1 | MME's handle — eNB uses this to route the response back |
| eNB-UE-S1AP-ID | 1 | Echo back so eNB knows which UE context this is |
| NAS: RAND | 16 bytes | eNB passes opaquely to UE |
| NAS: AUTN | 16 bytes | eNB passes opaquely to UE — UE verifies this |

**NAS message type: 0x52 = Authentication Request (TS 24.301 §8.2.7)**

---

### Step 5 — eNB → MME: UL NAS Transport (Auth Response)
**Spec: TS 36.413 §9.1.7.3**

In our simulator: eNB *simulates* UE computing `RES = RAND XOR 0x55` (simplified Milenage).
Real UE: USIM chip runs actual Milenage f2 function with Ki.

| NAS IE | Value |
|--------|-------|
| Message Type | 0x53 = Authentication Response |
| RES | 8 bytes (UE's computed response) |

**MME checks: RES == XRES → Authentication SUCCESS**

---

### Steps 6-9 — GTP-C Session Creation (UDP)

**Step 6: MME → S-GW: Create Session Request (TS 29.274 §7.2.1)**
**Port: UDP 2123 (MME sends from 2125)**

| GTP-C IE | Value | Why |
|----------|-------|-----|
| IMSI | 404100000000001 | S-GW forwards to P-GW for IP allocation |
| APN | "internet" | Which packet data network to connect to |
| EBI | 5 | EPS Bearer ID. 5 = default bearer. 6-15 = dedicated (VoLTE, gaming) |
| QCI | 9 | QoS Class Indicator. 9 = best-effort internet. 1 = VoLTE |
| AMBR UL/DL | 50/100 Mbps | Aggregate Max Bit Rate — caps total UE throughput |
| Sender FTEID | TEID=0 | Initial message, S-GW uses sender's address for response |

**What is a TEID?**
```
TEID = Tunnel Endpoint Identifier (32-bit number)
Like a port number but for GTP tunnels.

S-GW receives a GTP-U packet on its S1-U socket.
Reads the TEID in the packet header.
Looks up its table: "TEID 1001 → belongs to UE with IMSI 404100000000001"
Routes it to the correct UE / eNB.

TEID assigned by the RECEIVER.
eNB assigns its S1-U TEID → tells S-GW in Modify Bearer Request.
S-GW assigns its S1-U TEID → tells MME in Create Session Response.
```

**Step 7: S-GW → P-GW: Create Session Request**
S-GW forwards the request to P-GW on UDP:2124.
S-GW also allocates its own TEIDs:
- `sgw_s11_teid` (control plane, S11 interface with MME)
- `sgw_s1u_teid` (data plane, S1-U interface with eNB)

**Step 8: P-GW → PCRF: Diameter Gx CCR-I (TS 29.212 §4.5.1)**
**Port: TCP 3869**

Before creating the bearer, P-GW asks PCRF for permission.

| Gx IE | Value | Why |
|-------|-------|-----|
| IMSI | 404100000000001 | PCRF looks up subscriber's policy |
| APN | "internet" | Which service policy applies |
| QCI | 9 | Requested QoS class |

**PCRF responds with CCA-I:**
- Result: DIAMETER_SUCCESS (approved)
- Charging-Rule: "internet_rule" (what traffic is allowed)
- Approved bitrate (may be less than requested for throttled subscribers)

**INTERVIEW Q: "Why does P-GW need to ask PCRF before creating a bearer?"**
PCRF is the policy decision point. It can:
- Deny the session (prepaid subscriber with zero balance)
- Throttle the bitrate (congestion control)
- Apply service-specific rules (block certain ports, mirror traffic for lawful intercept)
- Trigger dedicated bearer for VoLTE (via RAR message, Phase 5)

**Step 9: P-GW allocates UE IP and responds**
P-GW assigns an IP from its pool: `10.0.0.1, 10.0.0.2, ...`

Create Session Response back to S-GW, then S-GW to MME:

| GTP-C IE | Value | Why |
|----------|-------|-----|
| Cause | 16 = Request Accepted | |
| S-GW S11 TEID | e.g. 1000 | MME uses this TEID to route future GTP-C to S-GW |
| S-GW S1-U TEID | e.g. 1001 | eNB uses this for data plane GTP-U tunnel to S-GW |
| UE IP | 10.0.0.1 | UE's internet address (included in Attach Accept) |

---

### Step 10 — MME → eNB: S1AP InitialContextSetupRequest
**Spec: TS 36.413 §9.1.4.1**

The biggest message. Carries:
1. Bearer setup parameters (so eNB can create GTP-U tunnel)
2. NAS Attach Accept (to deliver to UE)

| S1AP IE | Value | Why |
|---------|-------|-----|
| MME-UE-S1AP-ID | 1 | MME's handle |
| eNB-UE-S1AP-ID | 1 | eNB's handle |
| AMBR UL/DL | 50/100 Mbps | UE aggregate max bitrate |
| S-GW S1-U IP | 127.0.0.1 | eNB creates GTP-U tunnel to this address |
| S-GW S1-U TEID | 1001 | eNB puts this TEID in every uplink GTP-U packet |
| NAS: Attach Accept (0x42) | — | includes UE IP, TAI List, GUTI |
| NAS: UE IP | 10.0.0.1 | UE's internet address |

**REAL: also includes KeNB (eNB security key derived from Kasme)**
KeNB → eNB derives: KRRCenc, KRRCint (RRC encryption/integrity), KUPenc (user plane encryption)

---

### Step 11 — eNB → MME: S1AP InitialContextSetupResponse

eNB allocates its S1-U TEID and reports it to MME.

| IE | Value | Why |
|----|-------|-----|
| eNB S1-U TEID | e.g. 100 | S-GW puts this TEID in every downlink GTP-U packet to eNB |

---

### Step 12-13 — Modify Bearer Request/Response (GTP-C)
**MME tells S-GW: "the eNB's S1-U TEID is X"**

After this, S-GW's routing table is complete:
```
Downlink path: Internet → P-GW → S-GW → eNB (using eNB S1-U TEID) → UE
Uplink path:   UE → eNB → S-GW (using S-GW S1-U TEID) → P-GW → Internet
```

---

### Step 14 — eNB → MME: UL NAS Transport (Attach Complete)
**NAS message type: 0x46**

UE confirms it received Attach Accept and has activated the bearer.

**MME sets EMM state: SESSION_PENDING → REGISTERED**
UE now has full LTE data connectivity.

---

## 6. EMM State Machine

```
DEREGISTERED
    │  InitialUEMessage received (Attach Request)
    ▼
REGISTERED_INITIATED  ← UE context created
    │  Auth Request sent to UE
    ▼
AUTH_PENDING          ← waiting for UE's RES
    │  RES validated against XRES
    ▼
SESSION_PENDING       ← GTP-C Create Session in progress
    │  Attach Accept delivered, bearer confirmed
    ▼
REGISTERED            ← UE has IP, data connectivity live
    │  Detach Request (Phase 5)
    ▼
DEREGISTERED
```

---

## 7. How to Read the Wireshark Capture

### Step 1: Capture while running
```bash
sudo tcpdump -i lo0 \
  '(port 38412 or port 3868 or port 2123 or port 2124 or port 3869)' \
  -w ~/Desktop/mme_capture.pcap
```

### Step 2: Open in Wireshark
```bash
open ~/Desktop/mme_capture.pcap
```

### Step 3: Install the custom TLV dissector
1. Wireshark → Help → About → Folders → Personal Lua Plugins (find the path)
2. Copy `mme_sim_dissector.lua` to that folder
3. Wireshark → Analyze → Reload Lua Plugins
4. Now Wireshark decodes every field by name

### Step 4: Wireshark filters for each interface

| What you want to see | Filter |
|---------------------|--------|
| All meaningful traffic | `tcp.len > 0 or udp` |
| S1-MME: eNB ↔ MME only | `tcp.port == 38412 and tcp.len > 0` |
| Diameter: MME ↔ HSS only | `tcp.port == 3868 and tcp.len > 0` |
| GTP-C: MME ↔ S-GW only | `udp.port == 2123 or udp.port == 2125` |
| GTP-C: S-GW ↔ P-GW only | `udp.port == 2124` |
| Diameter Gx: P-GW ↔ PCRF | `tcp.port == 3869 and tcp.len > 0` |
| One UE's full attach | `tcp.len > 0 and tcp.port in {38412,3868,3869}` |

### Step 5: Identify each message by packet number

When you type `CR 1`, Wireshark will show packets in this order:

| Order | Direction | Port | Message | What to look for |
|-------|-----------|------|---------|-----------------|
| 1st data packet | → 38412 | TCP | InitialUEMessage | msg_type=0x0001 in first bytes after TCP header |
| 2nd | → 3868 | TCP | Diameter AIR | msg_type=0x0101 |
| 3rd | ← 3868 | TCP | Diameter AIA | msg_type=0x0102, followed by 16+16+8+32 bytes (RAND/AUTN/XRES/Kasme) |
| 4th | ← 38412 | TCP | DL NAS (Auth Req) | msg_type=0x0002, NAS type 0x52 |
| 5th | → 38412 | TCP | UL NAS (Auth Resp) | msg_type=0x0003, NAS type 0x53, 8 bytes RES |
| 6th | → 2123 UDP | UDP | Create Session Req | msg_type=0x0201 |
| 7th | → 2124 UDP | UDP | Create Session Req (S5) | msg_type=0x0201 |
| 8th | → 3869 | TCP | Diameter Gx CCR-I | msg_type=0x0401 |
| 9th | ← 3869 | TCP | Diameter Gx CCA-I | msg_type=0x0402 |
| 10th | ← 2124 | UDP | Create Session Rsp (S5) | msg_type=0x0202, contains UE IP (4 bytes: 10.0.0.1) |
| 11th | ← 2123 | UDP | Create Session Rsp (S11) | msg_type=0x0202, contains S-GW TEIDs + UE IP |
| 12th | ← 38412 | TCP | InitialContextSetupReq | msg_type=0x0004, contains S-GW S1U TEID + UE IP |
| 13th | → 38412 | TCP | InitialContextSetupRsp | msg_type=0x0005, contains eNB S1U TEID |
| 14th | → 2123 UDP | UDP | Modify Bearer Req | msg_type=0x0203, contains eNB TEID |
| 15th | ← 2123 | UDP | Modify Bearer Rsp | msg_type=0x0204, cause=16 |
| 16th | → 38412 | TCP | UL NAS (Attach Complete) | msg_type=0x0003, NAS type 0x46 |

### Step 6: Reading raw bytes without the dissector

Our wire format: `[4 bytes: payload length][2 bytes: msg_type][2 bytes: flags][4 bytes: seq_num][TLV IEs...]`

Each TLV: `[2 bytes: tag][2 bytes: length][N bytes: value]`

Example — first bytes of InitialUEMessage (TCP payload):
```
Bytes:   01 00 00 00  ← msg_type = 0x0001 (S1AP_INITIAL_UE_MSG) little-endian
         00 00        ← flags = 0
         01 00 00 00  ← seq_num = 1

Then TLVs:
         10 00        ← tag = 0x0010 (ENB_UE_S1AP_ID)
         04 00        ← length = 4
         01 00 00 00  ← value = 1 (eNB UE ID)

         06 01        ← tag = 0x0106 (NAS_IMSI)
         08 00        ← length = 8
         01 28 F4 DC 86 6F 01 00  ← IMSI = 404100000000001 little-endian uint64
```

**To find the IMSI in Wireshark:**
In the "Packet Bytes" panel at the bottom, look for the sequence `06 01 08 00` followed by 8 bytes — those 8 bytes are the IMSI in little-endian.

**To find the UE IP:**
Look for tag `08 03` (GTP_UE_IP = 0x0308) followed by 4 bytes — those 4 bytes are the IP (e.g., `0A 00 00 01` = 10.0.0.1)

---

## 8. Threading Model Explained Simply

```
THREAD          WHAT IT DOES                    LOCKS IT USES
──────────────────────────────────────────────────────────────
main            CLI: reads your commands         none
                Posts tasks to thread pool

enb_th          TCP server on 38412             cmd_mutex_ (command queue)
  └ rx_th       Receives from MME (DL NAS,ICSR) mme_send_mtx_ (shared socket write)

mme_th          Connects to eNB + HSS + SGW    pending_auth_mutex_ (auth handoff)
  └ hss_rx_th   Receives AIA from HSS           ue_store (sharded shared_mutex)
                Notifies mme_th via cv

hss_th          Diameter server on 3868         none (single-threaded receive loop)

sgw_th          UDP server on 2123              none (single-threaded)

pgw_th          UDP server on 2124              pcrf_conn (Gx TCP writes)

pcrf_th         TCP server on 3869              none

pool workers    Submit CR commands to eNB       cmd_mutex_ (same as above)
(8 threads)
```

**Key sync point — HSS auth handoff (Phase 2 pattern):**
```
mme_th (handleInitialUEMsg):
    sends AIR to HSS
    unique_lock lk(pending_auth_mutex_)
    pending_auth_cv_.wait(lk, [imsi]{ return pending_auth_.count(imsi); })
    ← BLOCKED HERE, sleeping, lock released ←

hss_rx_th (hssReceiveLoop):
    receives AIA from HSS
    lock_guard lk(pending_auth_mutex_)
    pending_auth_[imsi] = auth_vectors
    pending_auth_cv_.notify_all()
    
mme_th WAKES UP, gets auth vectors, continues
```

**Sharded UE store (Phase 3 pattern):**
```
64 buckets. Each bucket has its own shared_mutex.
UE with mme_id=37 → shard[37 % 64 = 37]

Read (STATUS command, main thread):
    shared_lock lk(shard[37].mutex)  ← multiple readers OK simultaneously

Write (MME adding new UE):
    unique_lock lk(shard[37].mutex)  ← exclusive, other shards unaffected

Result: 64x less contention than a single global mutex
```

---

## 9. Key Interview Questions and Answers

**Q: "Walk me through the 4G attach procedure"**

"UE sends Attach Request via eNB in an S1AP InitialUEMessage.
MME authenticates the UE by querying HSS for AKA auth vectors via Diameter S6a AIR.
HSS returns RAND/AUTN/XRES/Kasme. MME challenges UE with RAND+AUTN.
UE verifies AUTN (mutual auth) and returns RES. MME compares RES with XRES.
On auth success, MME queries S-GW to create a GTP-C session via S11 interface.
S-GW asks P-GW, which queries PCRF for policy and allocates a UE IP address.
MME delivers Attach Accept (with UE IP and bearer params) via S1AP InitialContextSetupRequest.
eNB sets up the radio bearer and data tunnel. MME closes the loop with Modify Bearer."

---

**Q: "What is a TEID and why do we need it?"**

"TEID is a 32-bit number that identifies a GTP tunnel endpoint.
Like a port number for UDP, it lets S-GW demultiplex packets from thousands of UEs arriving on one socket.
Assigned by the receiver: eNB assigns its TEID and tells S-GW via Modify Bearer.
S-GW assigns its TEID and tells eNB via Create Session Response.
Each UE has different TEIDs — this is how GTP-U routing works."

---

**Q: "What's the difference between S11, S5, and S1-U interfaces?"**

```
S11:  MME  ↔ S-GW  (GTP-C control plane, UDP 2123)
S5:   S-GW ↔ P-GW  (GTP-C control plane, UDP 2124)
S1-U: eNB  ↔ S-GW  (GTP-U user plane — actual data packets, UDP 2152)
S5-U: S-GW ↔ P-GW  (GTP-U user plane, UDP 2152)
SGi:  P-GW ↔ Internet
```

---

**Q: "What is the PCRF's role?"**

"PCRF is the policy decision point. Before P-GW creates a bearer, it asks PCRF via Diameter Gx CCR-I. PCRF checks the subscriber's policy: allowed APNs, max bitrate, online/offline charging. PCRF can deny (zero balance), throttle (congestion), or trigger dedicated bearers (VoLTE). In 5G this becomes PCF, using HTTP/2 (SBI) instead of Diameter."

---

**Q: "Why SCTP instead of TCP for S1AP in real 4G?"**

"SCTP gives three things TCP doesn't:
1. Message boundaries — each SCTP message is delivered whole (no length prefix needed)
2. Multi-streaming — each UE can get its own stream. One UE's retransmission doesn't block others
3. Multi-homing — eNB can have two IP addresses, failover if one path dies
We use TCP for simplicity, but the concepts are identical."

---

**Q: "What concurrency patterns did you use in this simulator?"**

"Six patterns, each phase adds one:
- mutex + condition_variable: CLI→eNB command queue (producer-consumer)
- atomic<bool>: stop flag for SIGINT graceful shutdown
- condition_variable handoff: HSS auth vectors from hss_rx_th to mme_th
- write mutex: commandLoop and receiveLoop sharing one TCP socket
- 64-shard shared_mutex: UE context store (reads dominate, writers isolated per shard)
- thread pool: BULK command fans out N attach tasks across 8 workers"

---

**Q: "What is the Flyweight pattern and where did you use it?"**

"In the simulator, thousands of UEs on the same APN would each store identical profile data (QCI, bitrate, charging flags). Flyweight separates intrinsic state (shared, immutable profile) from extrinsic state (per-UE IMSI, IP, TEID). All 'internet' UEs share one SubscriberProfile object via shared_ptr. Memory: N × sizeof(shared_ptr) instead of N × sizeof(profile). In production: subscriber profiles cached in Redis, all AMF pods share the cache."

---

## 10. Data Path After Attach (the user plane)

Once registered, data flows like this (NOT simulated — control plane only in our sim):

```
UE → eNB: radio (LTE Uu interface, RLC/MAC/PHY)
eNB → S-GW: GTP-U packet, contains eNB S1-U TEID in header (UDP 2152)
S-GW → P-GW: GTP-U packet, contains S-GW S5-U TEID in header (UDP 2152)
P-GW → Internet: regular IP packet (SGi interface, NAT applied)

Return path (downlink):
Internet → P-GW: IP packet addressed to UE's IP (10.0.0.1)
P-GW → S-GW: encapsulates in GTP-U with S-GW S5-U TEID
S-GW → eNB: encapsulates in GTP-U with eNB S1-U TEID
eNB → UE: radio delivery
```

Phase 5 of this simulator will add the GTP-U user plane simulation.

---

## 11. How to Add Your Own Features

**Adding a new message:**
1. Add the message type to `src/common/message_types.h`
2. Add any new IEs to `src/common/tlv.h`
3. Write the builder using `MessageWriter` in the sender node
4. Write the parser using `MessageReader` in the receiver node
5. Add the message type to `mme_sim_dissector.lua` for Wireshark decoding

**Adding Handover (Phase 5 idea):**
- X2 interface between eNBs (peer-to-peer, no MME involved)
- S1 handover via MME (when eNBs can't see each other)
- New messages: Handover Required, Handover Request, Handover Command, Path Switch Request

**VoLTE / IMS:** now implemented as the sibling project `../ims-simulator/`
(SIP REGISTER via P-CSCF, MTAS call control, dedicated QCI=1 bearer via Rx AAR).

---

## 12. Project Structure

```
4g-simulator/
├── CMakeLists.txt
├── mme_sim_dissector.lua        ← copy to Wireshark plugins folder
├── docs/
│   └── GUIDE.md                 ← this file
└── src/
    ├── main.cpp                 ← CLI, thread pool, BULK command
    ├── common/
    │   ├── message_types.h      ← enum of all message types
    │   ├── tlv.h                ← TLV encoding (MessageWriter/Reader)
    │   ├── socket_wrapper.h     ← RAII TCP + UDP sockets
    │   ├── thread_pool.h        ← 8-worker thread pool
    │   ├── subscriber_profile.h ← Flyweight pattern
    │   └── metrics.h            ← P95/P99 latency tracking
    ├── enb/ mme/ hss/ sgw/ pgw/ pcrf/
    │   └── *_node.h + *_node.cpp
    └── mme/
        ├── ue_context.h         ← per-UE state (bearer, auth vectors)
        └── ue_context_store.h   ← 64-shard shared_mutex store
```

---

*Built as an open learning project. Contributions welcome — add a new 3GPP procedure, write a Phase 5 feature, or port a node to Go.*
