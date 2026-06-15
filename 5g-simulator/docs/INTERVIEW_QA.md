# Interview Q&A — 5G Registration & SBA with Real Simulator Values

> These are exact values from our 5g-simulator codebase.
> Say this in the interview — confidently, with numbers.

---

## Q: "Explain the 5G Registration procedure."

**Say exactly this (word for word, practise it):**

---

"Sure. Registration is the 5G equivalent of the 4G Attach procedure — it's
how a UE gets onto the network and the AMF authenticates it. Let me walk
through it end to end with values from the 5G core simulator I built —
gNB, AMF, UDM, NRF, all talking real HTTP/1.1 + JSON over the Service-Based
Interface (SBI).

---

**Step 1 — RegistrationRequest [gNB → AMF, over N2]**

The UE doesn't send its permanent identity (SUPI/IMSI) over the air — it
sends a **SUCI**, the Subscription Concealed Identifier (TS 23.003 §28.7.2).
In our simulator, UE #1's SUCI is `suci-0-404-10-0000-0-0-0000000001` — that
breaks down as `suci-<supi type=0:IMSI>-<MCC=404>-<MNC=10>-<routing
indicator=0000>-<protection scheme=0:null>-<home network key id=0>-<MSIN>`.

We use 'null scheme' (protection scheme 0) — a real 3GPP-defined option
(TS 33.501 Annex C.2) for test networks where the scheme-output is just the
MSIN in plaintext. A real deployment encrypts the MSIN with the home
network's public key so a passive radio eavesdropper can't track the
subscriber.

The gNB wraps this in a `RegistrationRequest` JSON message and sends it to
the AMF over N2 — in our sim, TCP port `38412` (the real N2/NGAP port; real
N2 is NGAP over SCTP, we use length-prefixed JSON over TCP since SCTP isn't
practical on macOS — same simplification the 4G sim makes for S1AP).

AMF creates a `UeContext` keyed by `ranUeNgapId` (the gNB's reference for
this UE, TS 38.413) — `{suci, supi, xresStar}`.

---

**Step 2 — Nudm_UEAuthentication_Get [AMF → UDM, over SBI]**

AMF doesn't have the UE's keys — UDM does. AMF sends:

```
POST /nudm-ueau/v2/suci-0-404-10-0000-0-0-0000000001/security-information/generate-auth-data HTTP/1.1
Host: udm.5gc.mnc010.mcc404.3gppnetwork.org
```

This is real HTTP/1.1 with a JSON body — TS 29.509. In our pcap
(`5g_capture.pcap`), filter on `http` and you'll see this exact request and
its JSON response.

---

**Step 3 — UDM resolves SUCI → SUPI, returns 5G-AKA vector [UDM → AMF]**

Because we used the null scheme, UDM recovers the SUPI just by reading the
MSIN straight out of the SUCI — `imsi-404100000000001` — no decryption
needed. UDM looks up the subscriber's long-term key K, then returns:
- **RAND** — random challenge (16 bytes hex)
- **AUTN** — authentication token (placeholder in this sim — real AUTN =
  SQN⊕AK || AMF || MAC)
- **XRES\*** — expected response, computed here as `byteXor(RAND, K)` — our
  simplified stand-in for the real Milenage/5G-AKA `f2` function
- **KAUSF** — anchor key for further key derivation (TS 33.501)

Response is `200 OK` with a JSON body `{authType, supi, rand, autn,
xresStar, kausf}`. AMF caches `xresStar` in the UE's context — it's NEVER
sent to the UE.

---

**Step 4 — AuthenticationRequest / AuthenticationResponse [AMF ↔ gNB]**

AMF sends `AuthenticationRequest{rand, autn}` to the gNB over N2. The
simulated UE computes `RES* = byteXor(RAND, K)` using the same K — and sends
back `AuthenticationResponse{resStar}`.

AMF compares: `RES* == XRES*` → match → **5G-AKA mutual authentication
SUCCESS** (TS 33.501 §6.1.3.2). If they don't match, AMF replies with
`RegistrationReject{cause: "authentication-failure"}` — that path is in our
code too, not just the happy path.

---

**Step 5 — Nudm_SDM_Get [AMF → UDM, over SBI]**

Now that the UE is authenticated, AMF fetches subscription data:

```
GET /nudm-sdm/v2/imsi-404100000000001/am-data HTTP/1.1
```

`am-data` = "Access and Mobility" data (TS 29.503). UDM responds `200 OK`
with the subscribed S-NSSAI — `{sst:1, sd:"000001"}`, that's the **eMBB**
slice — and `subscribedUeAmbr: {uplink:"100Mbps", downlink:"200Mbps"}`. AMF
caches this so it doesn't re-query UDM on every later procedure.

---

**Step 6 — RegistrationAccept / RegistrationComplete [AMF ↔ gNB]**

AMF allocates a **5G-GUTI** — in our sim, formatted as
`5g-guti-404-10-amf01-00001` (MCC-MNC-AMF-region/identifier-UE sequence) —
and sends `RegistrationAccept{5gGuti, allowedNssai:[{sst:1,sd:"000001"}]}`
to the gNB. The gNB replies `RegistrationComplete`, and the UE is now
**registered on the 5G core**.

---

**Result — UE REGISTERED**

```
SUPI:        imsi-404100000000001
5G-GUTI:     5g-guti-404-10-amf01-00001
Slice:       S-NSSAI {sst:1, sd:"000001"}  (eMBB)
UE-AMBR:     100Mbps up / 200Mbps down
```

At this point a real network would proceed to **PDU Session Establishment**
(AMF → SMF → UPF, N4/PFCP) so the UE gets an IP — that's the next increment
on this project's roadmap, the 5G analogue of 'UE gets IP from P-GW' in the
4G EPC simulator."

---

## Q: "Why HTTP/JSON instead of Diameter (which 4G uses)?"

"This is one of the biggest architectural shifts from 4G to 5G — the move
to a **Service-Based Architecture (SBA)**, TS 23.501 §6.2. In 4G, MME, HSS,
PCRF etc. talk Diameter — a binary AVP-based protocol purpose-built for
telecom, running over SCTP/TCP.

In 5G, every core network function exposes a RESTful HTTP/2 API — the
Service-Based Interface (SBI). AMF calls UDM with `POST
/nudm-ueau/v2/{suci}/security-information/generate-auth-data` — that's a
normal HTTP request with a JSON body, the same tooling (curl, Postman,
API gateways, service meshes) that any web backend engineer already knows.

In our simulator this is real HTTP/1.1 + JSON (we don't implement HTTP/2
multiplexing, but the request/response shapes and URIs match TS 29.5xx).
You can literally read the JSON bodies in Wireshark — no custom dissector
needed, unlike Diameter."

---

## Q: "What is the NRF and how does service discovery work in 5G?"

"NRF — Network Repository Function (TS 23.501 §6.2.6, TS 29.510) — is the
'phone book' of the 5G core. Every NF registers its own profile on startup
and discovers peers by type instead of using hardcoded addresses. Two
operations:

- **Nnrf_NFManagement_NFRegister** — `PUT
  /nnrf-nfm/v1/nf-instances/{nfInstanceId}` with body `{nfInstanceId,
  nfType, host, port}` → `201 Created`.
- **Nnrf_NFDiscovery_Search** — `GET /nnrf-disc/v1/nf-instances?target-nf-type=UDM`
  → `200 OK` with that NF's `{host, port}`, or `404` if nothing's registered
  yet.

In our simulator, both UDM and AMF call `nrfclient::registerSelf()` on
startup (a shared helper, ~10x1s retry since startup order isn't
guaranteed), and AMF calls `nrfclient::discover(\"UDM\")` instead of using a
hardcoded `UDM_HOST` — falling back to the env var only if NRF discovery
404s. This IS the 'how do microservices find each other' story for 5G SBA —
it's the same pattern Kubernetes Services + DNS solve at the infra layer,
done here at the application/SBI layer, which is how a real telco core
does it too (NRF doesn't go away just because you're on K8s)."

---

## Q: "How does this compare to service discovery patterns you'd use in
microservices generally (e.g. Consul, Eureka, K8s DNS)?"

"Conceptually identical — NRF is 3GPP's domain-specific version of a service
registry. Register-on-startup, discover-by-type, fall back gracefully if the
registry is unreachable. The interesting wrinkle I hit building this: in our
K8s deployment (2 AMF replicas), one replica started before UDM had
registered with the NRF — got a 404, fell back to the K8s Service DNS name
`g5-udm-svc` (which still worked, since that's a stable name). The other
replica started slightly later, NRF discovery succeeded, and it got UDM's
address *from NRF* instead. Both code paths got exercised for real, in the
same deployment, just by timing — a good illustration of why you design for
graceful degradation instead of assuming discovery always succeeds."

---

## Q: "What's the AMF, and how does it map to the 4G MME?"

"AMF — Access and Mobility Management Function (TS 23.501 §6.2.1) — is the
5G evolution of the MME. It terminates N1 (NAS, to the UE) and N2 (NGAP, to
the gNB) — same role MME plays terminating NAS and S1AP. The big difference
is *how* it talks to the rest of the core: MME talks Diameter to HSS/PCRF;
AMF talks HTTP/JSON SBI to UDM (and, in later increments, SMF).

In our simulator, `amf_main.cpp` is structured exactly like the 4G sim's
`mme_node.cpp` — a `std::map<int, UeContext>` keyed by the UE's gNB
reference, holding `{suci, supi, xresStar}` between the RegistrationRequest
and AuthenticationResponse steps. The INTERVIEW_NOTE in the code calls out
explicitly: in a real horizontally-scaled AMF this map would live in a
shared UDSF (TS 23.501 §5.4.7.5) so any AMF replica can continue any UE's
procedure — ours is in-memory per-process, a documented simplification."

---

## Q: "Walk me through the Docker/Kubernetes setup for this project."

"Four binaries — `nrf_sim`, `udm_sim`, `amf_sim`, `gnb_sim` — each its own
Docker image via a 4-stage multi-target Dockerfile, same pattern as the 4G
simulator. On minikube I run all four as separate Deployments: 1 NRF
replica, 1 UDM replica, **2 AMF replicas**, **2 gNB replicas** — 6 pods
total.

Each pod has `tcpSocket` readiness/liveness probes — kubelet just checks the
SBI/N2 port accepts a connection, no HTTP health endpoint needed. PLMN
(MCC/MNC) comes from a shared ConfigMap (`plmn-configmap.yaml`) via
`envFrom`, so changing the operator's network ID doesn't require rebuilding
images — only restarting pods to pick up the new env.

I bring images up directly in minikube's Docker daemon (`eval $(minikube
docker-env)`) — no separate image-load step. After `kubectl apply -f k8s/`,
`kubectl get pods` shows all 6 Running with green probes, and the NRF's logs
show both AMF replicas' registration plus the UDM discovery race I mentioned
earlier. I also tested self-healing — `kubectl delete pod <gnb-pod>` — and
the replacement pod re-ran Registration for its UEs against the existing AMF
pods without any manual intervention."

---

## Q: "What is S-NSSAI / network slicing, in terms of what you've built?"

"S-NSSAI — Single Network Slice Selection Assistance Information (TS 23.501
§5.15) — identifies a network slice via SST (Slice/Service Type) + optional
SD (Slice Differentiator). SST=1 is the standardized value for **eMBB**
(enhanced Mobile Broadband) — normal high-throughput data, which is what
every UE in our simulator gets today (`{sst:1, sd:\"000001\"}`, returned in
both `Nudm_SDM_Get` and `RegistrationAccept`).

The next increment on this project's roadmap is making that configurable —
multiple S-NSSAI values (eMBB sst=1, URLLC sst=2 for low-latency, mMTC sst=3
for massive IoT) in the RegistrationRequest/PDU Session Establishment, and
NRF profiles becoming slice-aware (an NF can register 'I serve slice X').
That's the foundation of 5G's 'one physical network, many logical networks'
pitch — a URLLC slice for factory automation can get different QoS/priority
than an eMBB slice for video streaming, on the same infrastructure."

---

## Q: "What did you simplify, and why — be honest about it."

"A few documented simplifications, all called out in code comments:

1. **N2 is TCP with length-prefixed JSON, not NGAP over SCTP** — SCTP isn't
   practical to set up on macOS for a learning project; same call as the 4G
   sim's S1AP-over-TCP.
2. **5G-AKA crypto is `byteXor(RAND, K)`**, not real Milenage/`f1`-`f5`
   functions — the message *flow* (RAND/AUTN/RES*/XRES*/KAUSF, the
   comparison, the reject path) is real; the math inside is a stand-in.
3. **NRF keeps ONE registered instance per nfType**, not many — enough to
   demonstrate register-then-discover; a real NRF supports filtering across
   many instances.
4. **NRF traffic isn't in `5g_capture.pcap`** (only AMF writes that file,
   and two processes can't safely share one pcap) — it's fully visible in
   each node's session log instead.

I'd rather say 'here's exactly what's simplified and why' than have someone
discover it themselves and wonder what else might be hand-waved."
