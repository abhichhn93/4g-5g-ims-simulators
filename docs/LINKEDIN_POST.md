# LinkedIn Post — Copy & Paste Ready

---

## Post Version 1 — Technical Story

**I recreated what my team of 10 engineers built at Samsung — solo, in C++17.**

At Samsung R&D, I spent years working on 4G/5G core network systems. The attach procedure. Bearer setup. IMS registration. VoLTE calls. Each of those involves 7-8 nodes talking to each other across real protocols — GTP-C, Diameter, SIP, S1AP.

So I built it from scratch. Every node. Every message. Every IE.

**What the simulator includes:**

✅ eNB → MME → HSS (S6a Diameter — auth vectors, RAND/XRES/AUTN)
✅ MME → S-GW → P-GW (GTP-Cv2 — tunnel creation, TEID allocation)
✅ P-GW → PCRF (Diameter Gx — QCI policy, bearer authorization)
✅ P-CSCF → S-CSCF → IMS-HSS (SIP + Diameter Cx — VoLTE registration)
✅ MTAS invocation — call waiting, call barring, conference (MRFC/MRFP)
✅ Dedicated QCI=1 bearer via Rx interface — the actual VoLTE bearer

**The tech underneath:**
- C++17: `std::shared_mutex` sharding, thread pool, RAII sockets
- Binary TLV serialization (same structure as real 3GPP protocols)
- Color-coded live logs — watch packets flow between nodes in real time
- Wireshark capture — see actual TCP frames with custom Lua dissector

**Why it matters for learners:**
Every log line references the real 3GPP standard (TS 29.274, TS 29.272...).
A student can run `CR 1`, watch the full attach call flow in color, then open
Wireshark and match what they see to the 3GPP spec. That's how I would have
wanted to learn this.

GitHub: [link]

---

**Tags to add:**
`#4GLTE` `#VoLTE` `#IMS` `#Telecom` `#CPlusPlus` `#SystemsEngineering`
`#5G` `#NetworkEngineering` `#OpenSource` `#LearningInPublic`

---

## Post Version 2 — For Students/Freshers

**If you're preparing for a telecom systems interview, here's something that might help.**

I built an open-source 4G EPC + IMS/VoLTE simulator in C++17.

When you type `CR 1`:
→ eNB sends Attach Request to MME
→ MME sends AIR to HSS, gets back RAND + XRES + AUTN (EPS-AKA)
→ Security Mode Command sent to UE
→ Create Session flows through S-GW to P-GW
→ PCRF assigns QCI=9 policy (Diameter Gx)
→ Bearers come up, UE gets IP

Every step — color-coded, with the 3GPP TS reference in the log.
Then type `REGISTER` → full IMS registration in SIP.
Then `CALL` → VoLTE call with MTAS invocation and QCI=1 dedicated bearer.

Built it to connect my Samsung experience with C++ interview prep:
multithreading, smart pointers, design patterns, socket programming —
all in a real telecom context.

GitHub: [link]

---

**Screenshot captions to use:**

1. `CR 1` output — "Full 4G attach: eNB→MME→HSS→S-GW→P-GW→PCRF in ~50ms"
2. `REGISTER` output — "IMS registration: SIP REGISTER → Cx SAR/SAA → 200 OK"
3. `CALL` output — "VoLTE call setup: INVITE → MTAS → QCI=1 bearer via Rx AAR"
4. Wireshark screenshot — "Binary TLV packets visible in Wireshark with Lua dissector"
5. Project structure — "7 nodes, 6 protocols, C++17"

---

## Best time to post

- Tuesday/Wednesday 8-10am IST (highest LinkedIn engagement for tech posts)
- First comment: pin the GitHub link + "Drop a ⭐ if you find it useful"
