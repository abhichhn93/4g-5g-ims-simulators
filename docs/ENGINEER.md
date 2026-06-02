# Level 2 — Engineering Guide: 4G Attach Call Flow with 3GPP References

> For telecom students, freshers, 2-3yr engineers preparing for interviews.

---

## Architecture Overview

```
UE ──S1AP──► eNB ──S1AP──► MME ──Diameter S6a──► HSS
                              │
                         GTP-Cv2 S11 (UDP 2123)
                              │
                            S-GW ──GTP-Cv2 S5──► P-GW ──Diameter Gx──► PCRF
```

---

## Interface Summary

| Interface | Between | Protocol | Port | Standard |
|-----------|---------|----------|------|----------|
| S1-MME | eNB ↔ MME | S1AP over SCTP | 36412 | TS 36.413 |
| S6a | MME ↔ HSS | Diameter over SCTP | 3868 | TS 29.272 |
| S11 | MME ↔ S-GW | GTP-Cv2 over UDP | 2123 | TS 29.274 |
| S5 | S-GW ↔ P-GW | GTP-Cv2 over UDP | 2124 | TS 29.274 |
| Gx | P-GW ↔ PCRF | Diameter over SCTP | 3869 | TS 29.212 |
| Gm | UE ↔ P-CSCF | SIP over UDP/TCP | 5060 | RFC 3261 |
| Rx | P-CSCF ↔ PCRF | Diameter | 3868 | TS 29.214 |
| Cx | S-CSCF ↔ HSS | Diameter | 3868 | TS 29.229 |

> **Simulator note:** We use TCP instead of SCTP (macOS doesn't support SCTP
> natively). The application-layer encoding is protocol-accurate.

---

## 4G Attach — Full Call Flow (8 Steps)

### Step 1: Attach Request [UE → eNB → MME]
**3GPP ref:** TS 23.401 §5.3.2

UE sends NAS Attach Request embedded in S1AP Initial UE Message.

Key IEs:
| IE | Value | Why it matters |
|----|-------|---------------|
| IMSI | 404010000000001 | Subscriber identity (15 digits) |
| EPS Attach Type | 0x01 (Initial Attach) | vs Handover/Emergency |
| TAI | MCC=404, MNC=01, TAC=1 | Tracking Area — for paging |
| UE Network Capability | AMR-WB, ROHC... | What UE supports |

MME creates UE context, assigns MME-UE-S1AP-ID.

---

### Step 2: Authentication Information Request [MME → HSS]
**3GPP ref:** TS 29.272 §7.2.5 | Diameter Command Code: 318

MME sends AIR to HSS requesting authentication vectors.

Key AVPs (Diameter Attribute-Value-Pairs):
| AVP | Code | Value |
|-----|------|-------|
| User-Name | 1 | IMSI |
| Visited-PLMN-Id | 1407 | MCC+MNC |
| Requested-EUTRAN-Authentication-Info | 1408 | Num-Vectors=1 |

---

### Step 3: Authentication Information Answer [HSS → MME]
**3GPP ref:** TS 29.272 §7.2.6

HSS runs Milenage algorithm (f1..f5) using subscriber's Ki:

```
Input:  Ki (128-bit, stored in HSS/SIM), RAND (128-bit random)
Output:
  RAND → send to UE
  XRES → store in MME (compare with UE's RES)
  AUTN → send to UE (proves network is genuine)
  KASME → root key for NAS/AS security
  CK, IK → Cipher Key, Integrity Key
```

---

### Step 4: NAS Auth Request [MME → UE via eNB]
**3GPP ref:** TS 24.301 §8.2.7

MME wraps RAND + AUTN in S1AP Downlink NAS Transport.
UE receives it, runs Milenage with its SIM's Ki.

UE verifies AUTN — proves the network is real (prevents MITM/fake towers).
UE computes RES — proves UE has the real SIM.

---

### Step 5: NAS Auth Response [UE → MME]
**3GPP ref:** TS 24.301 §8.2.8

UE sends RES back. MME compares RES == XRES.
If match → **EPS-AKA Authentication SUCCESS**.

MME sends Security Mode Command:
- NAS Ciphering Algorithm: EEA1 (SNOW 3G) / EEA2 (AES)
- NAS Integrity Algorithm: EIA1 / EIA2

---

### Step 6: Create Session Request [MME → S-GW → P-GW]
**3GPP ref:** TS 29.274 §7.2.1 | GTPv2 Message Type: 32

MME sends Create Session Request to S-GW (GTPv2 over UDP).

Key IEs (GTPv2 Information Elements):
| IE | Type | Value |
|----|------|-------|
| IMSI | 0x01 | 404010000000001 |
| APN | 0x47 | internet |
| PDN Type | 0x57 | IPv4 (0x01) |
| Bearer Context | 0x5D | EBI=5, QCI=9 |
| MME S11 FTEID | 0x57 | IP + TEID |

S-GW forwards to P-GW (S5 Create Session Request).
P-GW queries PCRF via Diameter Gx (CCR Initial).

---

### Step 7: Initial Context Setup [MME → eNB]
**3GPP ref:** TS 36.413 §9.1.4.1

MME sends S1AP Initial Context Setup Request to eNB.

Key IEs:
| IE | Value |
|----|-------|
| UE-AMBR | 50 Mbps DL, 25 Mbps UL |
| E-RAB (Bearer) | EBI=5, QCI=9, S-GW S1-U TEID |
| NAS PDU | Attach Accept (contains IP address) |

eNB sets up radio bearer (DRB — Data Radio Bearer).
UE gets IP address in Attach Accept.

---

### Step 8: Attach Complete [UE → MME]
**3GPP ref:** TS 24.301 §8.2.4

UE sends Attach Complete → MME confirms → **REGISTERED**.

MME sends Modify Bearer Request to S-GW to activate charging.

**Result:**
- UE has IP address (allocated by P-GW)
- Default bearer (QCI=9) active — internet data flows
- UE EMM state: REGISTERED
- MME stores full UE context

---

## QCI Table

| QCI | Service | Max Latency | Notes |
|-----|---------|-------------|-------|
| 1 | VoLTE voice | 100ms | Dedicated bearer, highest priority |
| 5 | IMS signalling | 100ms | SIP packets to P-CSCF |
| 9 | Default internet | 300ms | YouTube, WhatsApp data |

---

## VoLTE — After Attach

After Attach Complete, UE has an IP. Now it registers with IMS:

```
UE ──SIP REGISTER──► P-CSCF ──► S-CSCF ──Cx SAR──► HSS
                                    └──ISC REGISTER──► MTAS
```

When VoLTE call made → MTAS invokes MRFC for conference/services.
P-CSCF sends Diameter Rx AAR to PCRF → PCRF creates QCI=1 bearer.

See `mme_ims` binary and `IMS_COMPLETE_GUIDE.md` for full IMS flows.
