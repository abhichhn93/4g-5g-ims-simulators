# LinkedIn Post — Copy & Paste Ready

---

## Post Version 1 — Technical Story (Service-Based Architecture / microservices angle)

**I built the 5G Core as 4 microservices — and watched them find each other
at runtime.**

After recreating the 4G EPC and IMS/VoLTE core solo in C++17 (see my last two
posts), I went one step further: the **5G Core's Service-Based Architecture
(SBA)** — the part of 5G that looks the least like "telecom" and the most
like a modern backend.

**What's in the simulator:**

✅ gNB → AMF over N2 — RegistrationRequest with a SUCI (concealed identity,
   TS 33.501)
✅ AMF → UDM over SBI — real HTTP/1.1 + JSON: `POST
   /nudm-ueau/v2/{suci}/security-information/generate-auth-data`
✅ 5G-AKA mutual authentication — RES* == XRES* before the UE is trusted
✅ AMF → UDM `Nudm_SDM_Get` for subscribed slice (S-NSSAI) + AMBR
✅ **NRF** — every NF registers itself on startup (`Nnrf_NFManagement_NFRegister`)
   and AMF *discovers* UDM by type (`Nnrf_NFDiscovery_Search`) — zero
   hardcoded addresses
✅ RegistrationAccept with 5G-GUTI + Allowed NSSAI

**The DevOps part:**
- 4 binaries (NRF, UDM, AMF, gNB), each its own container, each its own K8s
  Deployment
- On minikube: 1 NRF + 1 UDM + **2 AMF replicas** + **2 gNB replicas** = 6
  pods, readiness/liveness probes, ConfigMap-driven PLMN config
- Watched a real service-discovery race happen live: one AMF replica started
  before UDM had registered (404 → fell back to K8s DNS), the other found
  UDM via NRF directly — both paths, same deployment, just timing

**The tech underneath:**
- C++17, raw TCP sockets, multithreading
- HTTP/1.1 + JSON wire format — readable in Wireshark, no custom dissector
- pytest + tshark integration tests — every assertion from a real decoded pcap

GitHub: [link]

---

**Tags to add:**
`#5G` `#5GCore` `#Microservices` `#Kubernetes` `#Docker` `#CPlusPlus`
`#Telecom` `#SystemsEngineering` `#OpenSource` `#LearningInPublic`

---

## Post Version 2 — For Students/Freshers

**If you're trying to understand "what's actually new in 5G architecture",
here's something that might help.**

I built an open-source 5G Core simulator in C++17 — gNB, AMF, UDM, NRF —
that shows the **Service-Based Architecture** end to end.

When you run `REG 1`:
→ gNB sends RegistrationRequest with a SUCI (not the real IMSI!) to AMF
→ AMF asks UDM for a 5G-AKA auth vector — real HTTP POST with JSON body
→ UE computes RES*, AMF checks RES* == XRES* → authenticated
→ AMF asks UDM for subscription data (slice info, AMBR)
→ AMF issues a 5G-GUTI + allowed network slice (S-NSSAI)
→ UE is REGISTERED

The part most courses skip: **before any of this**, AMF and UDM both
registered themselves with the **NRF** — the 5G core's built-in service
registry. AMF then *discovered* UDM's address from the NRF instead of using
a hardcoded config. That's the actual mechanism behind "5G core is a set of
microservices" — and you can watch it happen in the terminal with
`LOG_LEVEL=ALL`.

Then I containerized all 4 nodes and ran 2 AMF + 2 gNB replicas on
Kubernetes — same NRF register/discover pattern, just with K8s pods instead
of bare processes.

Built it to connect 8 years of Samsung 4G/5G core experience with C++
interview prep — multithreading, smart pointers, RAII sockets, and now
service discovery / SBA patterns that show up in *any* microservices
interview, not just telecom.

GitHub: [link]

---

**Screenshot captions to use:**

1. `REG 1` output with `LOG_LEVEL=ALL` — "Full 5G Registration: SUCI →
   5G-AKA → NRF discovery → 5G-GUTI, with raw JSON on the wire"
2. NRF terminal log — "Every NF registers itself — AMF discovers UDM's
   address from the NRF, not a config file"
3. Wireshark on `5g_capture.pcap`, filter `http` — "Real HTTP/1.1 + JSON SBI
   traffic — Nudm_UEAuthentication_Get, Nudm_SDM_Get, readable without a
   custom dissector"
4. `kubectl get pods -o wide` — "6 pods: NRF, UDM, 2x AMF, 2x gNB — 5G core
   as Kubernetes microservices"
5. Project structure / architecture diagram — "4 NFs, 1 registry, real SBI"

---

## Best time to post

- Tuesday/Wednesday 8-10am IST (highest LinkedIn engagement for tech posts)
- Consider posting this as **Part 3** of a series (Part 1: 4G EPC, Part 2:
  IMS/VoLTE, Part 3: 5G Core SBA) — pin all three GitHub links in the first
  comment of each post so readers can follow the whole arc
- First comment: pin the GitHub link + "Drop a ⭐ if you find it useful"
