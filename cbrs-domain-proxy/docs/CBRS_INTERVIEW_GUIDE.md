# CBRS Interview Guide — Radisys / Domain Proxy Role

> Read this the night before. Every answer here is backed by something you built in the simulator.

---

## 1. What is CBRS? (say this in 30 seconds)

"CBRS is Citizens Broadband Radio Service — the 3.5 GHz spectrum band (3550–3700 MHz) in the US where spectrum is shared dynamically between three tiers: Navy radar incumbents at the top, PAL (licensed) operators in the middle, and GAA (free access) at the bottom. A cloud system called the SAS — Spectrum Access System — coordinates who gets to transmit and when, using a control plane protocol called WinnForum WINNF-TS-0016. CBRS is the spectrum layer that private 5G and O-RAN deployments run on."

---

## 2. What is a Domain Proxy? (your resume item)

"A Domain Proxy is middleware between CBSDs and the SAS. A CBSD is the base station — it could be a small cell in a factory, an outdoor antenna on a rooftop, or a private 5G node. Each CBSD must talk to the SAS to get spectrum authorization. But if you have 50 CBSDs in a building, you don't want 50 separate TLS connections to the cloud SAS — the Domain Proxy aggregates them into one connection, batches their requests, and enforces the CBRS state machine locally to reduce SAS round-trips.

In my simulator, the Domain Proxy:
- Accepts multiple CBSD connections on port 8700
- Maintains one persistent connection to the SAS on port 8800
- Validates state transitions before forwarding (e.g., rejects GrantRequest if CBSD is not REGISTERED)
- Tracks all CBSD states in a thread-safe registry"

---

## 3. CBRS State Machine (draw this on whiteboard)

```
UNREGISTERED
    │  RegistrationRequest → RegistrationResponse
    │  CBSD sends: FCC ID, location, height, antenna gain, category
    │  SAS assigns: cbsdId (globally unique)
    ▼
REGISTERED
    │  GrantRequest → GrantResponse
    │  CBSD asks for: center frequency, bandwidth, maxEirp
    │  SAS grants: grantId, channel, heartbeatInterval
    ▼
GRANTED       ← has channel assignment, NOT yet transmitting
    │  HeartbeatRequest → HeartbeatResponse
    │  CBSD confirms it's ready to transmit
    ▼
AUTHORIZED    ← actively transmitting (must heartbeat every 120s)
    │  RelinquishmentRequest OR grant expires
    ▼
REGISTERED
    │  DeregistrationRequest → DeregistrationResponse
    ▼
UNREGISTERED
```

**Interview Q: "Why the GRANTED state between REGISTERED and AUTHORIZED?"**

"Between GrantResponse and starting transmission, the CBSD needs to verify QoS preconditions — confirm the RF hardware is ready on the allocated channel. It's similar to VoLTE's `183 Session Progress` + `PRACK` precondition flow before ringing the callee. GRANTED means 'channel reserved for you' but AUTHORIZED means 'you may start transmitting now.' This two-phase approach lets SAS revoke grants before transmission starts without disrupting an active radio link."

---

## 4. What is the SAS? Who operates it?

"The SAS is a cloud-certified authority that:
1. Manages spectrum across all CBSDs in a geographic area
2. Protects US Navy radar incumbents via ESC (Environmental Sensing Capability) sensors on the East and West coasts
3. Coordinates PAL vs GAA channel assignments
4. Sends heartbeats to keep CBSDs honest about their location and transmit status

FCC-certified SAS operators: **Google SAS**, **Federated Wireless**, **CommScope**. My simulator (sas_stub) replicates their behavior for testing."

---

## 5. What is ESC? (likely asked at Radisys)

"ESC = Environmental Sensing Capability. A network of sensors — primarily on US coastlines — that detect when Navy radar systems (incumbents) start transmitting in the 3.5 GHz band. When the ESC detects radar activity, it signals the SAS, which sends `DEREGISTER` (responseCode=105) or `TERMINATED_GRANT` (code=400) to affected CBSDs in the radar's protection zone. The CBSDs must stop transmitting within 60 seconds — the Move List timer. This happens silently from the CBSD's perspective; it just gets a heartbeat failure code."

---

## 6. PAL vs GAA — explain the difference

| | PAL | GAA |
|-|-----|-----|
| What | Priority Access License | General Authorized Access |
| Who | Licensed operators (paid FCC auction) | Any CBSD with approval |
| Spectrum | 3550–3620 MHz (7 × 10 MHz blocks) | Full 3550–3700 MHz band |
| Priority | Protected from GAA interference | Opportunistic, can be displaced |
| Cost | Auction price + license fee | Free (equipment certification only) |

**Interview answer:** "In my simulator, the SAS stub always grants GAA. A real SAS would first check PAL allocations for the CBSD's location and frequency request — if the CBSD is asking for a PAL-licensed channel held by another operator, the SAS returns responseCode=302 (GRANT_CONFLICT). If the CBSD is in a PAL holder's protection zone and a PAL user shows up, the GAA CBSD gets a TERMINATED_GRANT on its next heartbeat."

---

## 7. CBSD Categories A and B

| | Category A | Category B |
|-|------------|------------|
| Power | ≤30 dBm EIRP | ≤47 dBm EIRP |
| Use case | Indoor, low-power (office small cells) | Outdoor, high-power (campus macro) |
| Location | Must be professionally installed if >30 dBm | Always professionally installed |
| Height | Typically <6m AGL | Up to 50m+ AGL |

"My CBSD-1 and CBSD-2 are Cat-A (office small cells at 3555 and 3565 MHz), and CBSD-3 is Cat-B (outdoor at 3580 MHz with 20 MHz bandwidth). The Domain Proxy enforces the EIRP cap: Cat-A maxEirp is capped at 30 dBm before forwarding to SAS."

---

## 8. How does CBRS connect to 5G/O-RAN?

"CBRS is the **spectrum authorization layer** for private 5G networks. The actual radio stack is O-RAN:

```
O-CU (Central Unit) ── F1 ── O-DU (Distributed Unit) ── Fronthaul ── O-RU (Radio Unit)
                                                                              │
                                                                    CBRS SAS client
                                                                    (Domain Proxy)
```

The O-RU is also the CBSD — it's the physical antenna that transmits on 3.5 GHz. Before the O-RU can transmit, the Domain Proxy must obtain a SAS grant for it. O-RAN WG4 specifies the fronthaul (O-DU ↔ O-RU) interface — this is the `Preferred Skills` item on the Radisys JD.

In my 5g-simulator, the `gnb_sim` is the gNB. In a production deployment, that gNB would also run a CBRS agent (like my `cbsd_agent`) to get SAS authorization."

---

## 9. What management functions did you implement? (Provisioning, Alarms)

From the JD: "Telecom management functions — Provisioning, Alarms, Software Management, Performance Management"

"In the Domain Proxy simulator, provisioning = CBSD Registration (FCC ID, location, antenna parameters). The SAS does the equivalent of HSS subscriber provisioning in 4G — it validates and assigns identity (cbsdId). 

For alarms: ESC-triggered TERMINATED_GRANT is an alarm event — CBSD must stop transmitting. In production, the Domain Proxy would publish this as a NETCONF alarm notification (RFC 6241). YANG models would describe the CBSD inventory and grant states as operational data. My simulator logs these events in `dp_session.log` as the hook point for NETCONF integration."

---

## 10. C++ design patterns in your implementation (they will ask)

| Pattern | Where | Why |
|---------|-------|-----|
| **RAII** | `Socket` class (socket_wrapper.h) | File descriptor closed in destructor — no leaks |
| **Thread per connection** | Domain Proxy accept loop | Each CBSD gets its own thread; `std::thread::detach()` |
| **Mutex + unordered_map** | `CbsdRegistry` | Shared CBSD state accessed by multiple CBSD threads |
| **State machine (enum)** | `SpectrumState` | Enforces legal transitions; rejected messages never reach SAS |
| **Singleton-style mutex** | `g_sas_mtx` | Only one CBSD talks to SAS at a time (serializes SAS requests) |
| **JSON over TCP** | wire.h + socket_wrapper.h | Zero dependencies; length-prefixed frames |

---

## 11. Key interview Q&A

**Q: "What is the heartbeat and why does it matter?"**
"The heartbeat is a periodic message the CBSD sends to SAS (every `heartbeatInterval` seconds, typically 120s). It serves two purposes: proves the CBSD is alive at its registered location, and refreshes the transmit window (`transmitExpireTime`). If the CBSD misses a heartbeat, SAS transitions the grant to TERMINATED_GRANT. The CBSD must stop transmitting within 60 seconds — called the Move List timer. This is critical for protecting Navy radar incumbents: if a CBSD goes rogue, SAS can force it offline via missed heartbeat."

**Q: "What happens when the ESC detects incumbent radar?"**
"SAS immediately sends TERMINATED_GRANT (code=400) or DeregistrationRequest (code=105) to all CBSDs in the protection zone. The Domain Proxy receives this on the SAS connection, looks up all affected CBSDs in its registry, and sends the error response to each CBSD. The CBSD must stop transmitting within 60s. In a real system, the Domain Proxy would also alert the operations center via NETCONF notification."

**Q: "How does your Domain Proxy handle multiple CBSDs?"**
"The Domain Proxy runs one accept thread that spawns a `std::thread` per CBSD connection. Each thread manages one CBSD's state machine. All CBSD state is in `CbsdRegistry` which uses a `std::mutex` for thread safety. The single SAS connection is protected by `g_sas_mtx` — only one CBSD's request goes to SAS at a time. A production DP would batch multiple requests into one SAS JSON array call for efficiency."

**Q: "What is NETCONF/YANG and how does it relate to CBRS?"**
"NETCONF (RFC 6241) is the network management protocol used to configure and monitor network devices — equivalent of REST/HTTP but for telecom equipment. YANG (RFC 7950) is the data modeling language for NETCONF schemas. In CBRS/O-RAN, the Domain Proxy exposes its CBSD inventory and grant states as YANG operational data. Operators query it via NETCONF to see: which CBSDs are registered, what spectrum is granted, current transmit state. I haven't implemented NETCONF yet — my simulator writes to log files as the management plane hook."

---

## 12. CBRS quick-reference card

```
Spectrum: 3550–3700 MHz  (150 MHz total, NR Band n48)
CBSDs:    Category A (≤30 dBm), Category B (≤47 dBm)
Protocol: WinnForum WINNF-TS-0016 (JSON/HTTPS)
SAS ops:  Google, Federated Wireless, CommScope
Incumbents protected: US Navy radar (SPN-43, SPY-1)
ESC: Environmental Sensing Capability sensors (coastal)

Messages: Registration → Grant → Heartbeat → Relinquishment → Deregistration
Ports (this sim): SAS=8800, Domain Proxy=8700 (←CBSDs)
```

---

*Strong candidate: 4G/5G core network experience + working CBRS Domain Proxy simulator + understanding of spectrum sharing architecture.*
