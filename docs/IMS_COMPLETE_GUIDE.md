# IMS / VoLTE Complete Guide — All Nodes, Flows, IEs
> Ericsson MTAS Interview Preparation

---

## 1. IMS Architecture Diagram (All Nodes)

```
                         ┌─────────────────────────────────────────────┐
                         │           IP Multimedia Networks (IMS)       │
                         │                                               │
  ┌────┐   Gm    ┌──────────┐  Mw  ┌──────────┐  Cx/Dx  ┌─────┐       │
  │ UE │◄───────►│  P-CSCF  │◄────►│  S-CSCF  │◄───────►│ HSS │       │
  └────┘         └──────────┘      └────┬─────┘         └──┬──┘       │
    │                 │                 │ISC              Sh │          │
    │ Ut              │ Rx              │                    │          │
    │                 ▼                 ▼                    │          │
    │              ┌──────┐       ┌──────────┐         ┌────┴──┐       │
    │              │ PCRF │       │  AS/MTAS │◄────────│  SLF  │       │
    │              └──────┘       └──────────┘         └───────┘       │
    │                                  │Rc                              │
    │                                  ▼                                │
    │                            ┌──────────┐  Cr  ┌──────────┐        │
    │              Mb             │  MRFC    │◄────►│   MRFP   │        │
    └────────────────────────────►│(conf ctrl)│     │(media mix)│        │
                  RTP             └──────────┘      └──────────┘        │
                                                                        │
  ┌──────────────────────────────────────────────────┐                  │
  │          CS/PSTN Interworking (CSFB/SRVCC)       │                  │
  │  ┌──────┐  Mg  ┌──────┐  Mk  ┌──────┐           │                  │
  │  │S-CSCF│─────►│ MGCF │─────►│ BGCF │           │                  │
  │  └──────┘      └──┬───┘      └──────┘           │                  │
  │                   │Mn                            │                  │
  │                   ▼                              │                  │
  │               ┌────────┐                         │                  │
  │               │IM-MGW  │◄── RTP ────► PSTN       │                  │
  │               └────────┘                         │                  │
  └──────────────────────────────────────────────────┘                  │
                                                                        │
  ┌──────────────────────────────┐                                      │
  │     Roaming / Interconnect   │                                      │
  │  ┌──────┐  Mx  ┌──────┐     │                                      │
  │  │I-CSCF│─────►│ IBCF │     │                                      │
  │  └──────┘      └──┬───┘     │                                      │
  │                   │Ix        │                                      │
  │                   ▼          │                                      │
  │               ┌──────┐       │                                      │
  │               │ TrGW │       │                                      │
  │               └──────┘       │                                      │
  └──────────────────────────────┘                                      │
```

---

## 2. Every Node Explained (Interview Ready)

### Core Nodes (must know)

| Node | Full Name | Role | Interface | Transport |
|------|-----------|------|-----------|-----------|
| **P-CSCF** | Proxy-CSCF | First SIP contact. Discovers via PCO in 4G attach. IPSec with UE. Sends Rx to PCRF for QCI=1 bearer | Gm (UE), Mw (S-CSCF), Rx (PCRF) | SIP/TCP or UDP 5060, TLS 5061 |
| **S-CSCF** | Serving-CSCF | Registrar + proxy. Applies iFC. Invokes MTAS via ISC. Core brain of IMS | Mw, ISC (AS), Cx (HSS) | SIP/TCP |
| **I-CSCF** | Interrogating-CSCF | Entry point for incoming calls/registrations from outside. Queries HSS for S-CSCF assignment | Mw, Cx (HSS), Dx (SLF) | SIP/TCP |
| **AS/MTAS** | Application Server | Service logic — VoLTE features, supplementary services. Ericsson MTAS is an AS | ISC (S-CSCF), Sh (HSS), Ut (UE) | SIP/TCP |
| **HSS** | Home Subscriber Server | IMS subscriber data, S-CSCF assignment, auth vectors | Cx, Sh, S6a (4G) | Diameter/SCTP |
| **SLF** | Subscriber Location Function | In multi-HSS deployments — tells I-CSCF which HSS has this user | Dx | Diameter |

### Media Nodes (MTAS/conferencing interviews)

| Node | Full Name | Role |
|------|-----------|------|
| **MRFC** | Media Resource Function Controller | Manages conference bridge. Controls MRFP via H.248/Megaco. Invoked by MTAS via Mr interface (SIP) |
| **MRFP** | Media Resource Function Processor | Does actual audio/video mixing. Terminates RTP from all participants. Sends mixed stream to each. Real-time processing |
| **MRB** | Media Resource Broker | Selects which MRFC/MRFP to use in large networks (load balancing across media resources) |

**INTERVIEW Q: "How does conferencing work in IMS?"**
"MTAS invokes MRFC via Mr interface (SIP). MRFC allocates a conference URI and instructs MRFP via H.248 to create a mixing endpoint. Each participant sends RTP to MRFP. MRFP mixes audio and sends personalized mixed streams back (everyone except yourself). MRFC manages the conference state."

### PSTN Interworking Nodes

| Node | Full Name | Role |
|------|-----------|------|
| **MGCF** | Media Gateway Control Function | Controls IM-MGW for PSTN interworking. Translates SIP↔ISUP (SS7 signalling). For calls to landlines/2G/3G |
| **IM-MGW** | IP Multimedia Media Gateway | Actual media conversion: RTP (IP) ↔ PCM (PSTN). Transcoding if needed |
| **BGCF** | Breakout Gateway Control Function | Routes calls that need to break out to PSTN. Selects MGCF based on routing |

### Roaming/Interconnect Nodes

| Node | Full Name | Role |
|------|-----------|------|
| **IBCF** | Interconnection Border Control Function | Border element for roaming or peering. Security, topology hiding, transcoding |
| **TrGW** | Transition Gateway | IPv4↔IPv6 translation for interconnect scenarios |

---

## 3. Transport Layer Details

| Interface | Between | Protocol | Port | Why |
|-----------|---------|----------|------|-----|
| Gm | UE ↔ P-CSCF | SIP over UDP/TCP | 5060 | UE to IMS signalling |
| Gm (secured) | UE ↔ P-CSCF | SIP over TLS | 5061 | IPSec/TLS for security |
| Mw | P-CSCF ↔ S-CSCF | SIP over TCP | 5060 | Core IMS signalling |
| ISC | S-CSCF ↔ MTAS | SIP over TCP | 5060 | AS invocation |
| Mr | S-CSCF/MTAS ↔ MRFC | SIP over TCP | 5060 | Conference control |
| Cr | MRFC ↔ MRFP | H.248/Megaco | 2944 | Media resource control |
| Cx | S-CSCF ↔ HSS | Diameter | 3868 | SCTP (TCP in our sim) |
| Rx | P-CSCF ↔ PCRF | Diameter | 3868 | SCTP |
| Sh | AS ↔ HSS | Diameter | 3868 | SCTP |
| Mb | MRFP ↔ UE | RTP/RTCP | Dynamic | Media stream |
| Mg | MGCF ↔ S-CSCF | SIP | 5060 | PSTN interworking |
| Mn | MGCF ↔ IM-MGW | H.248/Megaco | 2944 | Media GW control |

**Key point: SIP everywhere for signalling. RTP for media. Diameter for policies/auth.**

---

## 4. IMS Registration Flow (Step by Step)

```
UE                P-CSCF           I-CSCF        S-CSCF          HSS         MTAS
 │                    │                │               │              │            │
 │──REGISTER─────────►│                │               │              │            │
 │  From/To: sip:IMPU │                │               │              │            │
 │  Contact: 10.0.0.1 │                │               │              │            │
 │  Expires: 3600     │                │               │              │            │
 │                    │──REGISTER─────►│               │              │            │
 │                    │  + Via header  │──Cx UAR──────────────────────►│            │
 │                    │                │  "Who serves │this user?"    │            │
 │                    │                │◄─Cx UAA──────────────────────┤            │
 │                    │                │  S-CSCF=sip:scscf.domain     │            │
 │                    │                │──REGISTER────►│              │            │
 │                    │                │               │──Cx SAR─────►│            │
 │                    │                │               │  Register me │            │
 │                    │                │               │◄─Cx SAA──────┤            │
 │                    │                │               │  Profile+iFC │            │
 │                    │                │               │──3rdParty REGISTER────────►│
 │                    │                │               │  ISC interface            │
 │                    │                │               │◄─200 OK──────────────────┤│
 │◄───200 OK──────────┤◄───200 OK──────┤◄──200 OK──────┤              │            │
 │   P-Associated-URI │  Expires: 3600 │               │              │            │
 │   P-CSCF address   │                │               │              │            │
```

**Key IEs in REGISTER:**
- `From` = `To` = IMPU (sip: or tel: URI)
- `Contact` = UE's actual IP:port (from 4G bearer)
- `Authorization` = IMS-AKA challenge response
- `P-Access-Network-Info` = "3GPP-E-UTRAN-FDD; utran-cell-id-3gpp=..."
- Response `P-Associated-URI` = all your aliases (tel: + sip: URIs)

---

## 5. VoLTE Call Flow (Step by Step)

```
UE-A             P-CSCF-A        S-CSCF-A       MTAS        P-CSCF-B       UE-B
 │                   │                │              │             │             │
 │──INVITE──────────►│                │              │             │             │
 │ SDP offer:        │──INVITE───────►│              │             │             │
 │ audio AMR-WB      │                │──ISC INVITE─►│             │             │
 │                   │                │◄─continue────┤             │             │
 │                   │                │──INVITE──────────────────► │             │
 │                   │◄──100 Trying───┤◄─100 Trying──────────────── │             │
 │◄──100 Trying──────┤                │              │             │──INVITE────►│
 │                   │                │              │             │◄─180 Ringing┤
 │◄──180 Ringing─────┤◄──180 Ringing──┤◄─180 Ringing──────────────── │             │
 │  (play ringback)  │                │              │             │             │
 │                   │                │              │             │◄─200 OK─────┤
 │◄──200 OK──────────┤◄──200 OK───────┤◄─200 OK──────────────────── │  SDP answer │
 │  SDP answer       │                │              │             │             │
 │──ACK─────────────►│──ACK──────────►│──ISC ACK────►│             │             │
 │                   │                │              │──ACK────────────────────► │             │
 │                   │──Rx AAR──►PCRF │              │             │             │
 │                   │  QCI=1 request │              │             │             │
 │                   │◄─Rx AAA────────│              │             │             │
 │                   │  QCI=1 bearer created via 4G EPC            │             │
 │◄════RTP AMR-WB══════════════════════════════════════════════════════════════► │
 │  QCI=1 dedicated bearer          VOICE CALL ACTIVE              │             │
```

**Key IEs in INVITE:**
- `From` = caller's IMPU
- `To` = called party IMPU
- `Call-ID` = unique per dialog (used for all subsequent requests)
- `CSeq` = sequence number (1 INVITE, 2 ACK, 3 BYE...)
- `P-Preferred-Identity` = CLI to show callee
- `SDP m=audio` = RTP port + codec list
- `SDP a=rtpmap` = codec details (AMR-WB/16000)
- `SDP a=precondition` = QoS precondition (RFC 3312)

---

## 6. Call Waiting Flow

```
UE-A (active call with UE-B)     MTAS              UE-A
         │                          │                │
UE-C calls UE-A:                   │                │
UE-C──INVITE────────────────────►S-CSCF──ISC INVITE►│
                                    │                │
                              MTAS checks:           │
                              Is UE-A in a call?     │
                              YES → apply CW service │
                                    │                │
                              MTAS──re-INVITE───────►│
                              SDP: call-waiting=true  │
                              Sends 180 Ringing back  │
                                    │                │
                              UE-A hears BEEP         │
                              UE-A can:               │
                              a) Accept → put UE-B on HOLD, talk to UE-C
                              b) Reject → UE-C gets 486 Busy
                              c) Ignore → after timeout → voicemail
```

**Key:** MTAS knows UE-A is in a call because it tracks dialog state from the original INVITE.

---

## 7. Call Barring Flow

```
UE-A                P-CSCF         S-CSCF          MTAS
 │                      │               │               │
 │──INVITE──────────────►│──INVITE──────►│──ISC INVITE──►│
 │  To: +44xxx (UK)      │               │               │
 │                       │               │        MTAS checks barring rules:
 │                       │               │        User activated "Outgoing International Barring"
 │                       │               │        +44 = UK = International
 │                       │               │        → BARRED ✗
 │                       │               │◄─603 Decline──┤
 │◄──603 Decline─────────┤◄──603 Decline─┤               │
 │  Display: "Call barred"│               │               │
```

**Barring Types:**
| Code | Name | What it bars |
|------|------|-------------|
| OIB | Outgoing International Barring | All international calls |
| OIBH | Outgoing International Barring except Home | International except home PLMN |
| BAIC | Barring All Incoming Calls | All incoming |
| BIC-Roam | Barring Incoming when Roaming | Incoming while abroad |

---

## 8. Conference Call Flow (MRFC/MRFP)

```
UE-A (in call with UE-B) wants to add UE-C:

UE-A              P-CSCF         S-CSCF         MTAS         MRFC        MRFP
 │                    │               │              │             │           │
 │──re-INVITE────────►│──re-INVITE───►│──ISC re-INV─►│             │           │
 │  To: conference    │               │              │──Mr INVITE─►│           │
 │                    │               │              │  create conf│──H.248───►│
 │                    │               │              │             │ allocate  │
 │                    │               │              │◄─200 OK─────┤ mixer     │
 │                    │               │              │ conf-URI    │           │
 │                    │               │              │             │           │
 │                    │         MTAS sends INVITE to UE-B and UE-C            │
 │                    │         with conf-URI as Request-URI                   │
 │                    │               │              │             │           │
 │──────────────────────────────────RTP──────────────────────────►│           │
 │                                                                 │──mix─────►│
 │◄────────────────────────────────────RTP (mixed)────────────────┤           │
 │  Hears: UE-B + UE-C                                            │           │
```

**Key interfaces:**
- `Mr` = S-CSCF/MTAS → MRFC (SIP, TCP 5060)
- `Cr` = MRFC → MRFP (H.248/Megaco, port 2944)
- `Mb` = MRFP → UE (RTP)

**MRFC** = Controller (SIP state machine, decides what mixing to do)
**MRFP** = Processor (actual DSP work — mixing, transcoding, tone generation)

---

## 9. Connection to 4G EPC (The Full Story)

```
┌─────────────────────────────────────────────────────────────────┐
│                      YOUR mme_sim (Phase 1-4)                    │
│                                                                   │
│  CR 1 command:                                                    │
│  eNB──S1AP──►MME──Diameter──►HSS                                 │
│                │                                                  │
│                └──GTP-C──►SGW──GTP-C──►PGW──Gx──►PCRF           │
│                                          │                        │
│  Result: UE gets IP=10.0.0.1             │                        │
│          Default bearer QCI=9 active     │                        │
│          EMM state = REGISTERED          │                        │
└──────────────────────────┬──────────────┘                        │
                           │ UE has IP now                          │
                           ▼                                        │
┌─────────────────────────────────────────────────────────────────┐
│                    YOUR mme_ims (IMS layer)                       │
│                                                                   │
│  REGISTER command:                                                │
│  UE──SIP──►P-CSCF──SIP──►S-CSCF──Cx──►HSS                       │
│  Contact header uses IP from 4G (10.0.0.1)                       │
│                                                                   │
│  CALL command:                                                    │
│  INVITE → ... → 200 OK (media negotiated)                        │
│  P-CSCF──Rx AAR──►PCRF                                           │
│                     │                                             │
│                     └──Gx RAR──►PGW (our Phase 4 PCRF!)          │
│                                  │                                │
│                     Creates QCI=1 dedicated bearer                │
│                     RTP voice flows on QCI=1                      │
│                     Data continues on QCI=9                       │
└─────────────────────────────────────────────────────────────────┘
```

**Key IEs that link 4G EPC to IMS:**

| IE | Where | Links EPC to IMS |
|----|-------|-----------------|
| UE IP (10.0.0.1) | SIP Contact header | IP allocated by P-GW in EPC is used in IMS registration |
| QCI=1 | Dedicated bearer | PCRF creates this when P-CSCF sends Rx AAR after INVITE |
| P-CSCF address | PCO in PDN Connectivity | EPC tells UE which P-CSCF to use during 4G attach |
| IMSI | Both HSS databases | Same HSS serves S6a (4G) and Cx (IMS) interfaces |
| MSISDN | HSS → S-CSCF via SAA | Phone number linked to IMS identity |

---

## 10. How to Capture and View IMS Logs

### Capture:
```bash
# Terminal 1 — capture IMS ports
sudo tcpdump -i lo0 \
  '(port 5060 or port 5070 or port 3870)' \
  -w ~/Desktop/ims_capture.pcap

# Terminal 2 — run IMS simulator
cd /Users/abhichauhan/Desktop/cpp-interview-prep/mme-simulator/build
./mme_ims

# Type: REGISTER → CALL → BYE → QUIT
```

### Wireshark filters:
```
# All IMS traffic
tcp.port in {5060, 5070, 3870} and tcp.len > 0

# Just SIP signalling
tcp.port in {5060, 5070} and tcp.len > 0

# Just Diameter Cx (HSS)
tcp.port == 3870 and tcp.len > 0
```

### What to look for in each packet:

| Packet | Filter | What you see |
|--------|--------|-------------|
| 1st data | port 5060 | REGISTER (msg_type=0x0501) |
| 2nd | port 3870 | Cx SAR (msg_type=0x0603) |
| 3rd | port 3870 | Cx SAA with subscriber profile |
| 4th | port 5060 | 200 OK for REGISTER |
| 5th | port 5060 | INVITE (msg_type=0x0502) |
| 6th | port 5060 | 100 Trying |
| 7th | port 5060 | 180 Ringing |
| 8th | port 5060 | 200 OK with SDP answer |
| 9th | port 5060 | ACK |

---

## 11. Quick Interview Q&A

**Q: "What is the difference between P-CSCF, I-CSCF, and S-CSCF?"**
"P-CSCF is the edge proxy — first SIP hop from UE, handles IPSec and sends Rx to PCRF.
I-CSCF is the entry point for registrations/calls from outside — queries HSS to find S-CSCF.
S-CSCF is the core — the registrar, applies service triggers (iFC), invokes MTAS."

**Q: "What is MRFC and how does it relate to MTAS?"**
"MRFC is the controller for media resources — conferences, announcements, tones.
MTAS invokes MRFC via the Mr interface (SIP) when conference service is needed.
MRFC instructs MRFP (the actual processor) via H.248. MTAS manages the service logic,
MRFC manages the conference state, MRFP does the actual audio mixing."

**Q: "How is a VoWiFi call different from VoLTE at the IMS level?"**
"Identical at IMS level. P-CSCF doesn't know or care if UE is on LTE or WiFi.
The difference is the bearer: VoLTE uses QCI=1 LTE bearer, VoWiFi uses ePDG+IPSec.
MTAS handles both transparently."

**Q: "What happens when a VoLTE user roams to another country?"**
"UE attaches to visited network EPC (gets IP). P-CSCF is in visited network.
P-CSCF routes SIP REGISTER via I-CSCF in HOME network (DNS discovery).
I-CSCF queries HOME HSS, assigns HOME S-CSCF. S-CSCF invokes HOME MTAS.
IBCF sits at the boundary — hides topology, handles interop."

**Q: "How does SRVCC work?"**
"Single Radio Voice Call Continuity — when UE moves from VoLTE to 2G/3G.
MME detects weak LTE signal, triggers SRVCC handover to CS domain.
EPC hands RTP bearer to MSC via IM-MGW. MTAS is involved in the anchor transfer.
User doesn't hear interruption (target <300ms switchover)."
