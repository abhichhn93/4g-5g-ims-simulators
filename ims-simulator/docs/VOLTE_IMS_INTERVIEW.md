# VoLTE / IMS Interview Guide — Ericsson MTAS
> For Senior Technical Lead interview. Read this the night before.

---

## 1. What is IMS? (say this in 30 seconds)

"IMS — IP Multimedia Subsystem — is the control plane framework for delivering
voice, video, and messaging over IP networks. In 4G/VoLTE, the UE gets an IP
address via the EPC data bearer, then uses that IP to register with IMS using SIP.
Calls are established via SIP INVITE. The actual voice travels as RTP packets over
a dedicated QCI=1 bearer that the EPC sets up on PCRF's instruction."

---

## 2. IMS Architecture — Nodes and Roles

```
UE ──SIP──► P-CSCF ──SIP──► I-CSCF ──Cx──► HSS
                  │                │
                  │            UAR/UAA (find S-CSCF)
                  │                │
                  └──SIP──► S-CSCF ──Cx──► HSS (SAR/SAA)
                                │
                           ISC (SIP)
                                │
                            MTAS (AS) ← Ericsson's Application Server
                                │
                          Supplementary Services:
                          OIP, OIR, CLIP, CLIR,
                          Call Waiting, Forwarding,
                          Conference, VoLTE codec policy
```

| Node | Full Name | Role |
|------|-----------|------|
| P-CSCF | Proxy-CSCF | First SIP contact. UE discovers via PCO in 4G attach. Sends Rx to PCRF for QCI=1 bearer |
| I-CSCF | Interrogating-CSCF | Queries HSS (UAR) to find which S-CSCF serves this user |
| S-CSCF | Serving-CSCF | SIP registrar. Applies iFC. Invokes MTAS via ISC interface |
| MTAS | Multimedia Telephony AS | Ericsson's AS. Handles VoLTE services, supplementary services |
| HSS | Home Subscriber Server | IMS subscriber data. Cx interface to S-CSCF |
| PCRF | Policy and Charging | Receives Rx from P-CSCF → triggers QCI=1 bearer via P-GW |

---

## 3. IMS Registration Flow (memorise this)

```
UE → P-CSCF:  SIP REGISTER
               From/To: sip:+919...@ims.domain
               Contact: sip:ue@10.0.0.1:5060   ← UE's 4G IP!
               Expires: 3600

P-CSCF → I-CSCF:  forward REGISTER
                   adds Via header (route tracing)

I-CSCF → HSS:   Diameter Cx UAR (User-Authorization-Request)
                 "Which S-CSCF should serve this user?"
HSS → I-CSCF:   Diameter Cx UAA
                 Returns: S-CSCF name

I-CSCF → S-CSCF: forward REGISTER

S-CSCF → HSS:   Diameter Cx SAR (Server-Assignment-Request)
                 "I am now serving this user, give me their profile"
HSS → S-CSCF:   Diameter Cx SAA
                 Returns: subscriber profile + iFC (Initial Filter Criteria)
                 iFC says: "Invoke MTAS on REGISTER and INVITE"

S-CSCF → MTAS:  Third Party REGISTER (via ISC interface)
                 MTAS stores: user's IMPU, service profile, contact

S-CSCF → I-CSCF → P-CSCF → UE:  SIP 200 OK
                                   Expires: 3600
                                   P-Associated-URI: tel:+919...
```

**INTERVIEW Q: "What is iFC?"**
"Initial Filter Criteria — stored in HSS, delivered to S-CSCF via SAA.
Defines WHEN S-CSCF should invoke an Application Server like MTAS.
Example: 'If method=REGISTER, invoke MTAS'. Trigger Points match
SIP headers/methods/bodies. This is how Ericsson MTAS gets invoked
without S-CSCF knowing what services exist."

---

## 4. VoLTE Call Setup Flow (memorise this)

```
UE-A → P-CSCF:    SIP INVITE
                   From: sip:+919000000001@ims
                   To:   sip:+919000000002@ims
                   SDP:  audio RTP/UDP, codec=AMR-WB, port=50000

P-CSCF → S-CSCF:  forward INVITE
S-CSCF → MTAS:    ISC INVITE (3rd party call control)
                   MTAS checks: OIP, call barring, forwarding

MTAS → S-CSCF:    continue (or modify, or reject)
S-CSCF → P-CSCF-B → UE-B:  forward INVITE
UE-B → P-CSCF-B:  SIP 100 Trying
                   SIP 180 Ringing  ← caller hears ringback
                   SIP 200 OK + SDP answer
                   codec=AMR-WB, port=50002

200 OK goes back to UE-A
UE-A → UE-B:      SIP ACK (call confirmed)

P-CSCF → PCRF:    Diameter Rx AAR  ← KEY STEP
                   "Media negotiated: audio AMR-WB 12.65kbps"
PCRF → P-GW:      Gx RAR (install QCI=1 charging rule)
P-GW → MME:       Create Bearer Request (QCI=1, ARP=2)
MME → eNB:        S1AP E-RABSetupRequest
eNB sets up DRB with QCI=1 → voice bearer live

RTP flows: UE-A ←────────────────────── UE-B
           QCI=1 dedicated bearer (priority over data)
```

---

## 5. Key IEs to Know

### SIP REGISTER IEs
| Header | Example | Why |
|--------|---------|-----|
| From/To | sip:+919...@ims.domain | Same — registering your own identity |
| Contact | sip:ue@10.0.0.1:5060 | UE's actual IP:port for incoming calls |
| Expires | 3600 | Re-register before this. If 0 → deregister |
| Authorization | Digest + IMS-AKA | Authentication challenge response |
| P-Access-Network-Info | 3GPP-E-UTRAN-FDD;utran-cell-id | Tells IMS which cell UE is on |

### SIP INVITE IEs
| Header | Example | Why |
|--------|---------|-----|
| From | sip:+919...@ims | Caller identity |
| To | sip:+919...@ims | Called party |
| Call-ID | abc123@10.0.0.1 | Unique per call dialog |
| CSeq | 1 INVITE | Sequence number for ordering |
| P-Preferred-Identity | tel:+919000000001 | CLI to present to called party |
| SDP: m= audio | audio 50000 RTP/AVP 98 | RTP port + codec |
| SDP: a=rtpmap | 98 AMR-WB/16000 | Codec definition |

### SDP Codec Negotiation (VoLTE)
```
Offer (UE-A):
  m=audio 50000 RTP/AVP 98 99
  a=rtpmap:98 AMR-WB/16000   ← preferred: HD voice
  a=rtpmap:99 AMR/8000        ← fallback: standard

Answer (UE-B accepts AMR-WB):
  m=audio 50002 RTP/AVP 98
  a=rtpmap:98 AMR-WB/16000    ← both agree on AMR-WB
```

---

## 6. Diameter Cx Interface (S-CSCF ↔ HSS)

| Command | Direction | Purpose |
|---------|-----------|---------|
| UAR/UAA | I-CSCF → HSS | Find S-CSCF for this user |
| SAR/SAA | S-CSCF → HSS | Register, get subscriber profile + iFC |
| LIR/LIA | I-CSCF → HSS | Find where user is registered (for call routing) |
| MAR/MAA | S-CSCF → HSS | Get IMS auth vectors (IMS-AKA challenge) |
| RTR/RTA | HSS → S-CSCF | HSS tells S-CSCF to de-register user |

---

## 7. Diameter Rx Interface (P-CSCF ↔ PCRF)

**When triggered:** After SDP negotiation complete (200 OK to INVITE received)

| Command | Direction | Purpose |
|---------|-----------|---------|
| AAR | P-CSCF → PCRF | "I have a media session, install QCI=1 bearer" |
| AAA | PCRF → P-CSCF | Confirmed, bearer will be created |
| STR | P-CSCF → PCRF | "Call ended, release the QCI=1 bearer" |
| RAR | PCRF → P-CSCF | PCRF-initiated (e.g., emergency, policy change) |

**Rx AAR carries:**
- Media-Component: codec, bandwidth, port range, direction
- Subscription-ID: IMPU (links to the Gx session on P-GW)

**Chain reaction:** Rx AAR → PCRF → Gx RAR to P-GW → P-GW creates bearer → MME → eNB → QCI=1 DRB

---

## 8. MTAS — What Ericsson Expects You to Know

**What MTAS is:**
Ericsson's Multimedia Telephony Application Server. An IMS Application Server (AS)
invoked by S-CSCF via the ISC (IMS Service Control) interface.

**How MTAS is invoked:**
1. S-CSCF receives REGISTER or INVITE
2. S-CSCF checks iFC (downloaded from HSS via SAR/SAA)
3. iFC says: "Invoke MTAS for this trigger condition"
4. S-CSCF sends SIP REGISTER/INVITE copy to MTAS
5. MTAS processes, applies service logic, returns 200 OK or modified SDP

**What MTAS does:**

| Service | What it means |
|---------|---------------|
| OIP/OIR | Originating Identity Presentation/Restriction — show/hide CLI |
| TIP/TIS | Terminating identity — show/hide called party number |
| CLIP/CLIR | Calling Line ID Presentation/Restriction (equivalent of above) |
| Call Waiting | Notify UE of incoming call when already on a call |
| Call Forwarding | Forward to voicemail, another number, on busy/no-answer |
| CONF | Multi-party conference (add 3rd party to call) |
| MMTEL | Multimedia Telephony — umbrella service feature set |
| Codec Policy | Enforce AMR-WB for VoLTE, reject non-compliant codec offers |

**MTAS and 3rd Party Registration:**
When UE registers, S-CSCF sends a copy of REGISTER to MTAS.
MTAS stores the user's registration, enabling it to:
- Know when user is reachable (for push notifications on iOS/Android)
- Apply per-user service settings

---

## 9. QCI Values for VoLTE

| QCI | Service | Latency | Loss | Priority |
|-----|---------|---------|------|----------|
| 1 | Conversational Voice (VoLTE) | 100ms | 10^-2 | Highest |
| 2 | Conversational Video | 150ms | 10^-3 | High |
| 5 | IMS Signalling (SIP) | 100ms | 10^-6 | High |
| 9 | Default data (internet) | 300ms | 10^-6 | Low |

**INTERVIEW Q: "Why does VoLTE need a dedicated bearer (QCI=1)?"**
"The default bearer (QCI=9) is best-effort — your voice packets compete with
YouTube and WhatsApp. QCI=1 gives strict priority scheduling in eNB and S-GW.
The eNB scheduler ensures voice packets always go out first, guaranteeing
<100ms latency. Without it, packet loss and jitter would make voice unusable."

---

## 10. VoWiFi (Voice over WiFi) — Same IMS, Different Access

"VoWiFi uses the same IMS infrastructure as VoLTE. The difference is the access:
instead of going through the 4G RAN and EPC, UE connects via WiFi → ePDG
(evolved Packet Data Gateway) → IMS. The SIP REGISTER is identical.
P-CSCF doesn't know or care — it just sees a SIP REGISTER with a WiFi IP.
Ericsson MTAS handles both transparently — it only sees IMS signalling."

---

## 11. Key Interview Questions for Ericsson MTAS Role

**Q: "What is MTAS and how is it different from S-CSCF?"**
"S-CSCF is a SIP proxy/registrar — it routes SIP messages and applies service
triggers from iFC. MTAS is an Application Server where the actual service LOGIC lives.
S-CSCF decides WHEN to call MTAS (based on iFC). MTAS decides WHAT to do
(apply supplementary services, modify SDP, generate CDRs)."

**Q: "How would you design MTAS for carrier-grade reliability?"**
"Active-active clustering with shared state in distributed DB (Redis/Cassandra).
Each MTAS instance handles a subset of calls — consistent hashing by Call-ID.
If one fails, another picks up mid-call using stored dialog state.
Heartbeat detection via SIP OPTIONS or Diameter Watchdog. Geo-redundancy
across two data centers. Target: 5 nines (99.999% = 5 min downtime/year)."

**Q: "What is the ISC interface?"**
"IMS Service Control — the SIP interface between S-CSCF and Application Servers.
Standard SIP (RFC 3261). S-CSCF acts as B2BUA (Back-to-Back User Agent) toward AS.
MTAS uses ISC to receive service triggers, apply logic, return modified requests."

**Q: "How does call forwarding work in IMS?"**
"MTAS receives INVITE from S-CSCF via ISC. Checks subscriber's forwarding rules
(stored in MTAS from Sh interface with HSS or local DB). If UE is busy or not
registered, MTAS generates a new INVITE to the forwarding target. S-CSCF
sees this as a new call leg. Original caller doesn't know it was forwarded
(unless MTAS adds a Diversion header)."

**Q: "How does VoLTE handle handover between cells?"**
"S1 handover doesn't affect IMS — the RTP bearer moves between eNBs at EPC level,
the SIP dialog remains unchanged. X2 handover is seamless. Inter-frequency
handover may cause a brief RTP gap (100-200ms) — acceptable for voice.
SRVCC (Single Radio Voice Call Continuity): when UE moves to 2G/3G, MME triggers
handover and EPC hands the IMS call to CS domain (MGW + MSC). MTAS is involved
in the anchor point transfer."

**Q: "What is AMR-WB and why is it used for VoLTE?"**
"Adaptive Multi-Rate Wideband. Operates at 16kHz sampling (vs 8kHz for narrowband).
Covers 50-7000Hz frequency range (human voice fundamental + harmonics).
12.65kbps bitrate — very efficient. Result: HD Voice — caller sounds like in the room.
VoLTE mandates AMR-WB support. eNB gives QCI=1 bearer enough bandwidth."

---

## 12. Connection to Your 4G EPC Simulator

Your mme_sim (Phases 1-4) implements the EPC part. mme_ims implements IMS.
Together they show the complete VoLTE story:

```
Phase 1-4 (mme_sim):
  CR 1 → Attach Request → Auth → Bearer Setup → UE gets IP=10.0.0.1
                                                         │
                                                    That IP is used here ↓
IMS (mme_ims):
  REGISTER → UE registers at P-CSCF with Contact: sip:ue@10.0.0.1
  CALL → SIP INVITE → MTAS services → 200 OK → ACK
               │
        Rx AAR → PCRF → QCI=1 bearer (via our Phase 4 PCRF/P-GW!)
```

**In the interview:** "I built a complete 4G EPC + IMS simulator from scratch in C++17.
The EPC handles: Attach, AKA auth (Diameter S6a), GTP-C bearer setup, PCRF policy.
The IMS layer handles: SIP REGISTER flow, S-CSCF with MTAS invocation, Rx interface
for QCI=1 dedicated bearer. All using real TCP/UDP sockets — you can see packets
in Wireshark."

---

*Good luck tomorrow, Abhi. Samsung background + this simulator = strong candidate.*
