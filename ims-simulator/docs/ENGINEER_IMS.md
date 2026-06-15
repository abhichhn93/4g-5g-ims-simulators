# IMS/VoLTE — Engineering Guide: Call Flows, IEs, Interview Q&A

---

## Architecture

```
UE ──SIP/TCP:5060──► P-CSCF ──SIP:5060──► I-CSCF ──Cx──► HSS
                         |                      |
                         |                 UAR/UAA
                         |                      |
                      Rx(Dia)              ──── S-CSCF ──Cx SAR/SAA──► HSS
                         |                      |ISC
                      PCRF                  MTAS (Ericsson AS)
                         |                      |Mr
                      Gx↓                    MRFC ──H.248──► MRFP
                      P-GW
                      QCI=1
```

---

## IMS Registration — Full IE Table

### SIP REGISTER (UE → P-CSCF)

| Header | Example Value | Why It Matters |
|--------|--------------|----------------|
| From / To | `sip:+919000000001@ims.mnc010.mcc404.3gppnetwork.org` | IMPU — public identity |
| Contact | `sip:ue@10.0.0.1:5060` | **UE's 4G IP** — from P-GW during EPC attach! |
| Expires | `3600` | UE re-registers before this. If 0 → deregister |
| P-Access-Network-Info | `3GPP-E-UTRAN-FDD; utran-cell-id-3gpp=404010001` | Tells IMS UE is on 4G LTE |
| Authorization | `Digest realm="ims.domain", nonce="..."` | IMS-AKA challenge response |
| Via | `SIP/2.0/TCP 10.0.0.1:5060;branch=z9hG4bKreg1` | Route tracing — each proxy adds its Via |

**INTERVIEW Q: "IMPU vs IMPI?"**
- IMPU (Public): `sip:+919...@ims.domain` — like a phone number, shown to others
- IMPI (Private): `919...@ims.domain` — like a username, used for authentication only

### Diameter Cx SAR/SAA (S-CSCF ↔ HSS)

| AVP | Value | Why |
|-----|-------|-----|
| Server-Assignment-Type | REGISTRATION (1) | S-CSCF tells HSS: I'm serving this user |
| User-Name | IMPU | Which user |
| iFC (in SAA) | `<TriggerPoint><Method>REGISTER</Method>→invoke MTAS` | When to call MTAS |
| MSISDN | +919000000001 | Phone number linked to IMPU |

**iFC = Initial Filter Criteria** — this is CRITICAL for Ericsson MTAS interviews.
HSS stores iFC per subscriber. SAA delivers them to S-CSCF.
S-CSCF applies iFC: "if method=INVITE, invoke MTAS AS via ISC interface."

### SIP 200 OK (Registration Response)

| Header | Value | Why |
|--------|-------|-----|
| P-Associated-URI | `tel:+919000000001` | All aliases for this identity |
| Service-Route | `sip:scscf.ims.domain;lr` | Future requests MUST go through this S-CSCF |
| Contact + Expires | `3600` | Confirmed registration lifetime |

---

## VoLTE Call Setup — Full IE Table

### SIP INVITE (Caller UE → P-CSCF → S-CSCF)

| Header | Example | Why |
|--------|---------|-----|
| From | `sip:+919000000001@ims.domain;tag=inv1` | Caller identity + dialog tag |
| To | `sip:+919000000002@ims.domain` | Called party (no tag yet — dialog not established) |
| Call-ID | `call-AB-1@10.0.0.1` | **Unique per dialog** — ALL messages in this call share it |
| CSeq | `1 INVITE` | Sequence number — detects retransmits and ordering |
| P-Preferred-Identity | `sip:+919000000001@ims.domain` | CLI to show callee |
| Supported | `100rel,precondition` | Reliable provisional responses (PRACK) |
| SDP m=audio | `50000 RTP/AVP 98 99` | RTP port + codec list (offer) |
| SDP a=rtpmap:98 | `AMR-WB/16000` | HD Voice codec — 16kHz sampling |
| SDP a=rtpmap:99 | `AMR/8000` | Fallback codec (narrowband) |
| SDP a=sendrecv | — | Bidirectional (vs sendonly=hold, recvonly, inactive) |

### SIP 180 Ringing

| Header | Example | Why |
|--------|---------|-----|
| To-tag | `;tag=callee1` | **DIALOG ESTABLISHED HERE** — this tag + From-tag + Call-ID = dialog ID |
| Contact | `sip:ue-b@10.0.0.2:5060` | Where to send future in-dialog requests |

### SIP 200 OK (INVITE — callee answers)

| Header | Example | Why |
|--------|---------|-----|
| SDP m=audio | `60000 RTP/AVP 98` | **SDP ANSWER** — one codec selected from offer |
| SDP a=rtpmap:98 | `AMR-WB/16000` | AMR-WB chosen — HD Voice negotiated |
| Contact | `sip:ue-b@10.0.0.2:5060` | Callee's contact for ACK |

**INTERVIEW Q: "What is SDP negotiation?"**
- INVITE carries SDP offer: "I support AMR-WB, AMR, G.711"
- 200 OK carries SDP answer: "I choose AMR-WB"
- RFC 3264 — Offer/Answer Model
- Both ends now use AMR-WB, same RTP ports agreed

### SIP ACK

Confirms 200 OK received. Now P-CSCF triggers dedicated bearer.

### Diameter Rx AAR (P-CSCF → PCRF)

| AVP | Value | Why |
|-----|-------|-----|
| Media-Component | codec=AMR-WB, bw=12650bps, dir=sendrecv | Media description from SDP |
| Subscription-Id | IMPU | Links to existing Gx session on P-GW |
| Reservation-Priority | `DEFAULT` | For emergency calls: `EMERGENCY` |

**This is the critical EPC↔IMS link:**
- P-CSCF sends Rx AAR to PCRF after SDP negotiation complete
- PCRF already has Gx session with P-GW (from 4G attach)
- PCRF sends Gx RAR to P-GW: "install QCI=1 rule for this UE"
- P-GW creates dedicated QCI=1 bearer → voice travels on this

---

## MTAS — Ericsson Interview Topics

### What MTAS does when INVITE arrives (via ISC):

1. **OIP** (Originating Identity Presentation): Is caller allowed to show CLI? Yes → proceed
2. **Call Barring**: Is OIB active? Is called number international? → barred → 603 Decline
3. **MMTEL service**: Is call forwarding active? Forward if UE unreachable
4. **CDR**: Start Charging Data Record (for billing)
5. **Codec policy**: Enforce AMR-WB for VoLTE quality

### Third Party Registration (3PCC):

When UE registers, S-CSCF sends copy of REGISTER to MTAS via ISC.
MTAS stores: IMPU + contact + service profile.
Enables MTAS to know if UE is reachable (for push notifications).

### ISC interface (S-CSCF ↔ MTAS):

Standard SIP. S-CSCF acts as B2BUA toward MTAS.
MTAS can: modify SDP, reject call (603), redirect, inject itself in media path.

---

## Conference — MRFC/MRFP

### Flow:
```
UE-A re-INVITE → S-CSCF → MTAS → MRFC (Mr interface, SIP)
MRFC → MRFP (Cr interface, H.248/Megaco, port 2944)
MRFP: allocate 3-party mixing endpoint
MTAS sends INVITE to UE-B, UE-C with conference URI
All RTP → MRFP → mixed → each UE
```

### Key distinction:
| Node | Interface | Protocol | Role |
|------|-----------|----------|------|
| MTAS | ISC (to S-CSCF) | SIP | Service logic — when/how to conference |
| MRFC | Mr (to MTAS/S-CSCF) | SIP | Conference state machine |
| MRFP | Cr (to MRFC) | H.248 | DSP — actual audio/video mixing |

---

## Call Waiting

UE-A active call → UE-C calls UE-A:
1. S-CSCF → MTAS: INVITE for UE-A
2. MTAS checks: active dialog exists for UE-A
3. MTAS sends re-INVITE to UE-A with `Alert-Info: <sip:beep>` (call waiting beep)
4. UE-A can: Accept (HOLD UE-B via SDP `a=inactive`) | Reject (486 Busy) | Ignore (timeout→voicemail)

**Key SDP difference for HOLD:**
- Active call: `a=sendrecv`
- On hold: `a=sendonly` (UE still receives, doesn't send)
- Full hold: `a=inactive`

---

## Call Barring Types (all handled by MTAS)

| Code | Name | What it blocks |
|------|------|---------------|
| OIB | Outgoing International Barring | All international |
| OIBH | Outgoing International except Home | International except home PLMN |
| BAIC | Barring All Incoming Calls | All incoming |
| BIC-Roam | Barring Incoming when Roaming | Incoming while abroad |

All result in: MTAS → S-CSCF → `603 Decline` → caller gets "Call Barred".

---

## Transport Layer Summary

| Interface | Between | Protocol | Port |
|-----------|---------|----------|------|
| Gm | UE ↔ P-CSCF | SIP/TCP (or UDP) | 5060 |
| Mw | P-CSCF ↔ S-CSCF | SIP/TCP | 5060 |
| ISC | S-CSCF ↔ MTAS | SIP/TCP | 5060 |
| Mr | S-CSCF/MTAS ↔ MRFC | SIP/TCP | 5060 |
| Cr | MRFC ↔ MRFP | H.248/Megaco | 2944 |
| Cx | S-CSCF/I-CSCF ↔ HSS | Diameter/SCTP | 3868 |
| Rx | P-CSCF ↔ PCRF | Diameter/SCTP | 3868 |
| Mb | MRFP ↔ UE | RTP/UDP | dynamic |

---

## Wireshark Filters

```
sip                          → all SIP (REGISTER, INVITE, 200 OK, ACK, BYE)
diameter                     → Cx SAR/SAA, Rx AAR/AAA
sip.Method == "INVITE"       → only INVITE requests
sip.Status-Code == 180       → only 180 Ringing
sip.Call-ID contains "call-" → filter by call ID
```
