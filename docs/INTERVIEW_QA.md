# Interview Q&A — 4G Attach Flow with Real Simulator Values

> These are exact values from our mme-simulator codebase.
> Say this in the interview — confidently, with numbers.

---

## Q: "Explain the LTE Attach procedure."

**Say exactly this (word for word, practise it):**

---

"Sure. The attach procedure is how a UE gets registered to the network and receives an IP address to send data. Let me walk you through it end to end — I'll tie it to values from the 4G EPC simulator I built.

---

**Step 1 — RRC + Attach Request [UE → eNB → MME]**

The UE powers on and does RRC Connection Setup with the eNB on the radio side. Once RRC is up, the UE sends a NAS Attach Request. This carries the IMSI — in our simulator we use IMSI `404010000000001`, which is MCC=404, MNC=01, a standard Airtel India PLMN format. The Attach Type is `0x01` — Initial Attach. The UE also sends its EPS security capabilities and the APN it wants — in our case `internet`.

The eNB doesn't read the NAS — it's opaque. The eNB wraps it into an S1AP InitialUEMessage and sends it over SCTP to the MME on port `38412`. Along with the NAS, the eNB adds the TAI — MCC=404, MNC=01, TAC=1 — and its local eNB-UE-S1AP-ID.

MME creates a UE context. In our code, we use a sharded store — 64 buckets, each with its own `shared_mutex` — and assign a MME-UE-S1AP-ID, which is the MME's local handle for this UE.

---

**Step 2 — Diameter AIR to HSS [MME → HSS]**

MME sends an Authentication-Information-Request over the S6a interface — Diameter over TCP port `3868` in our simulator (real networks use SCTP). Command Code is `318`. The Diameter Application-ID is `16777251` — that's the 3GPP S6a application.

The AIR carries the IMSI and the Visited PLMN ID. In our PCAP file (`mme_capture.pcap`), if you filter on `diameter`, you'll see this as the first Diameter packet with the R-flag set — that's the Request bit in the Diameter header.

---

**Step 3 — HSS returns AIA with Auth Vectors [HSS → MME]**

The HSS runs the Milenage algorithm using the subscriber's Ki. It generates one authentication vector:
- **RAND** — a 128-bit random challenge, e.g. `A3F2B1C4D5E60000...` (you see this in our step banner logs)
- **XRES** — 64-bit expected response, e.g. `8C1D2E3F4A5B6C7D` — MME stores this
- **AUTN** — 128-bit auth token the UE uses to verify the network is real
- **KASME** — 256-bit root key derived from CK and IK

The AIA comes back on the same TCP connection, Command Code `318`, R-flag cleared (Answer).

---

**Step 4 — NAS Authentication Request [MME → UE via eNB]**

MME sends RAND and AUTN down to the UE wrapped in an S1AP DownlinkNASTransport, NAS message type `0x52`. The eNB just forwards it on the radio — still opaque to eNB.

UE's USIM verifies AUTN — this is mutual authentication. Unlike 2G/3G where only the network authenticated the UE, in 4G the UE also verifies the network is legitimate. This prevents IMSI catchers and rogue base stations.

If AUTN is valid, the UE computes RES from RAND + Ki using Milenage.

---

**Step 5 — NAS Authentication Response [UE → MME]**

UE sends NAS Auth Response, message type `0x53`, carrying RES. In our simulator you can see the actual hex value in the step banner logs.

MME compares: `RES == XRES` → match → **EPS-AKA Authentication SUCCESS**.

MME sends NAS Security Mode Command — picks EEA2 (AES-128) for ciphering, EIA2 (AES-128 CMAC) for integrity. The SMC itself is integrity-protected but NOT encrypted — because you can't encrypt the message that tells the UE to start encrypting.

UE activates the algorithms, sends Security Mode Complete. All NAS from this point is integrity-protected and ciphered.

---

**Step 6 — Create Session Request [MME → S-GW → P-GW]**

MME sends a GTPv2 Create Session Request to the S-GW. In our simulator this goes over UDP on port `2123` — that's the S11 interface. The MME's own S11 UDP port is `2125`.

Key IEs in the Create Session Request:
- IMSI: `404010000000001`
- APN: `internet`
- PDN Type: IPv4 (`0x01`)
- Bearer Context: EBI=`5` (EPS Bearer ID — 5 is always the default bearer), QCI=`9`
- MME S11 F-TEID: MME's own control-plane TEID so S-GW knows where to respond

In our PCAP file, filter `gtpv2` — you'll see this as Message Type `32` (Create Session Request) with TEID present and the GTPv2 Version=2 flag set.

S-GW allocates its own TEIDs and forwards to P-GW on the S5 interface — port `2124` in our simulator.

---

**Step 7 — P-GW allocates IP, queries PCRF [P-GW → PCRF]**

P-GW allocates a UE IP address. In our implementation, P-GW has an IP pool starting at `10.0.0.1`, allocated using `atomic<uint32_t>` with `fetch_add` — lock-free. First UE gets `10.0.0.1`, second gets `10.0.0.2`, and so on up to `10.0.254.254`.

P-GW sends a Diameter Credit-Control-Request Initial — CCR-I — to the PCRF on port `3869`. Command Code `272`, Application-ID `16777238` — that's the 3GPP Gx application. The CCR carries the APN, IMSI, and the requested QCI.

PCRF looks up the subscriber profile. In our Flyweight pattern implementation, all UEs on the `internet` APN share ONE `SubscriberProfile` object via `shared_ptr`. The profile returns:
- QCI=`9`
- Max DL = `100 Mbps` (100,000,000 bps)
- Max UL = `50 Mbps` (50,000,000 bps)

PCRF sends Credit-Control-Answer — CCA — back to P-GW approving the session.

P-GW sends Create Session Response back through S-GW to MME, containing:
- UE IP: `10.0.0.1`
- P-GW S5 TEID (user plane)
- S-GW S1-U TEID (the eNB will send user-plane data here)

---

**Step 8 — Initial Context Setup [MME → eNB]**

MME now has everything. It sends S1AP InitialContextSetupRequest to the eNB. This is the key S1AP message — it carries:
- The NAS Attach Accept (inside which is the UE's IP `10.0.0.1`, the allocated GUTI, TAI list)
- E-RAB Setup: EBI=`5`, QCI=`9`, S-GW S1-U transport address and TEID
- UE-AMBR: `100 Mbps` DL / `50 Mbps` UL
- KeNB security key for radio-level encryption

eNB sets up the radio Data Radio Bearer (DRB), sends RRC Connection Reconfiguration to UE. UE acknowledges with RRC Connection Reconfiguration Complete.

eNB responds to MME with InitialContextSetupResponse carrying the eNB's own S1-U TEID. MME sends a GTPv2 Modify Bearer Request to S-GW so the downlink path is complete.

Finally, UE sends NAS Attach Complete (message type `0x46`) containing Activate Default EPS Bearer Context Accept.

---

**Result — UE REGISTERED**

At this point in our simulator, the step 8 banner shows:
```
| STEP 8/8  ATTACH COMPLETE  UE REGISTERED        [T-XXXXX] |
| UE -----------------------------------------> eNB -> MME   |
| [UE: AUTHENTICATED -> REGISTERED]                          |
| [MME: SESSION_PENDING -> REGISTERED]                       |
|   IMSI:    404010000000001                                  |
|   UE IP:   10.0.0.1  (allocated by P-GW)                   |
|   QCI-9:   Default bearer ACTIVE                           |
|   Latency: ~45ms  (full attach end-to-end)                 |
```

The user-plane GTP-U tunnel is now active:
```
UE ──radio──► eNB ──GTP-U:UDP:2152──► S-GW ──GTP-U:UDP:2152──► P-GW ──SGi──► Internet
                    TEID=eNB's TEID        TEID=S-GW's TEID
```

The UE can now send and receive IP data. If you run `./mme_ims` next (in the
`../ims-simulator/` sibling project), the UE registers with IMS using
`10.0.0.1` as the SIP Contact address, and when a VoLTE call is made, the
PCRF creates a dedicated QCI=`1` bearer on top of this default bearer."

---

## Q: "What is a TEID and why do we need it?"

"TEID is Tunnel Endpoint Identifier — a 32-bit number that acts like a port number for GTP tunnels. Without it, S-GW wouldn't know which UE a downlink packet belongs to, because all packets from P-GW arrive on the same UDP socket. The TEID in the GTP-U header tells S-GW: 'this packet belongs to UE with TEID X, forward it toward eNB on its S1-U tunnel.'

In our simulator, TEIDs are allocated with `atomic<uint32_t> next_teid_` and `fetch_add` — completely lock-free, no mutex needed, because each TEID is independent."

---

## Q: "Why SCTP for S1AP and not TCP?"

"Three reasons.
First — multi-streaming. SCTP supports multiple independent streams in one association. If a large message on stream 1 is being retransmitted, messages on stream 2 still flow. TCP has head-of-line blocking — one stalled packet blocks everything.
Second — multi-homing. An SCTP association can use multiple IP addresses. If the primary path fails, SCTP automatically fails over to the backup — critical for carrier-grade reliability.
Third — message boundaries. SCTP is message-oriented like UDP, unlike TCP which is a byte stream. Each S1AP message arrives as a complete message — no need to implement your own framing. In our simulator we use TCP and implement framing manually with a 4-byte length prefix."

---

## Q: "What is QCI=9 vs QCI=1?"

"QCI — QoS Class Indicator — defines the scheduling priority, latency budget, and loss tolerance for a bearer.
QCI=9 is the default bearer — internet data. Best-effort, 300ms latency budget. Your YouTube, WhatsApp, everything goes here.
QCI=1 is for VoLTE voice. Strictly prioritized scheduling in the eNB, 100ms latency budget, 10^-2 packet loss rate. The eNB always sends QCI=1 packets first.
In our simulator, the default bearer (EBI=5) is QCI=9. When you run `./mme_ims`
(in `../ims-simulator/`) and make a VoLTE CALL, the P-CSCF sends a Diameter Rx
AAR to the PCRF, which triggers a new Create Bearer Request for a dedicated
QCI=1 bearer — EBI=6."

---

## Q: "What is the Flyweight pattern in your MME simulator?"

"When you have 1000 UEs all on the APN 'internet', each UE context would normally hold a full copy of the subscriber profile — QCI, bitrates, charging flags, etc. That's wasteful.
With Flyweight, we have ONE `SubscriberProfile` object per APN, stored in `ProfileRegistry`. Each UE context holds a `shared_ptr<SubscriberProfile>` — just 8 bytes instead of a full copy. 1000 UEs = 8KB of shared_ptr overhead instead of ~100KB of duplicate profile data.
The profile is immutable — `const` members — so shared access needs no locking."

---

## Q: "How does your sharded UE context store work?"

"A global mutex on the UE store would serialize all lookups — killing throughput for BULK attach. So we shard: 64 buckets, each with its own `shared_mutex`. A UE with MME-ID=N goes into bucket `N % 64`.
`shared_mutex` allows concurrent readers — multiple MME handler threads can look up different UEs in the same shard simultaneously. Only writes (insert, update) take exclusive lock. Result: ~64x less contention on the hot path."
