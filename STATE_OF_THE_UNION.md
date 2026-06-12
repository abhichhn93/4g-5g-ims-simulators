# THE TELECOM-SYSTEMS ARCHITECT MANIFESTO: 4G/5G/IMS SIMULATOR

## 1. STRATEGIC VISION & PORTFOLIO GOAL
This project is a high-performance, multi-protocol EPC/IMS simulation suite designed to showcase senior-level expertise in **Systems Programming**, **3GPP Protocol Mastery**, and **Cloud-Native Architecture**.

**Strategic Goal:** To demonstrate "Industry-Ready" engineering by solving real-world telecom problems (Head-of-Line blocking, signaling storms, IMSI cleartext vulnerabilities) using Modern C++ patterns.

---
 
## 2. SENIOR C++ DESIGN PATTERNS (THE "SHOW-OFF" LIST)
To satisfy a 7-year experience interview, the codebase implements:

### A. Exception Safety & Thread-Resilience
- **Pattern:** Custom `TelecomException` hierarchy.
- **Logic:** Carrier-grade nodes cannot crash. We wrap thread entry points in `try-catch` blocks. If an eNB sends a corrupt packet, we catch it, log the hex, and keep the node running.

### B. Memory Ownership (Modern RAII)
- **Pattern:** Zero use of `new`/`delete`. Everything is managed via `std::unique_ptr` and `std::shared_ptr`.
- **Logic:** Eliminates memory leaks and "Double Free" bugs, which are common 3GPP-node pitfalls.

### C. Concurrency & Synchronization
- **Pattern:** `std::atomic` for lifecycle, `std::shared_mutex` for sharded reads, and `std::async` for non-blocking service lookups (MTAS).
- **Logic:** Demonstrates ability to handle "Signaling Storms" without global lock contention.

### D. Polymorphism & Operator Overloading
- **Pattern:** `BaseNode` interface and `operator<<` for protocol headers.
- **Logic:** Decouples protocol parsing from logging, allowing for easy expansion to 5G NGAP/SBI.

---

## 3. TELECOM DOMAIN DEEP-DIVE

### A. Transport Abstraction (The SCTP Problem)
- **Technical Fact:** S1AP requires SCTP (TS 36.413). macOS lacks native SCTP support.
- **Solution:** We implement a **Message-Framing Shim** over TCP. 
- **Interview Answer:** "I am aware that production MMEs use SCTP; for this simulator, I implemented a 4-byte length prefix framing to mimic SCTP's record-oriented delivery over a TCP stream. The PcapWriter then maps these back to SCTP headers for Wireshark."

### B. IMS & VoLTE Logic
- **iFC (Initial Filter Criteria):** Logic in S-CSCF to trigger MTAS.
- **Rx Interface (Diameter):** Linkage between SIP signaling and EPC bearer management.

---

## 4. THE TRIPLE-TRACK ENGINE
1.  **Track 1 (C++):** Core Protocol Stack. Focus on bit-manipulation and memory.
2.  **Track 2 (Go):** 5G Service Based Architecture (SBA). Focus on NRF Discovery.
3.  **Track 3 (Python):** Automation. Focus on `pyshark` for CI/CD validation.

---

## 5. EXPERIENCED INTERVIEW Q&A

**Q: "How do you handle thread contention in the UE store?"**
*Answer:* "Instead of a global lock, I implemented a sharded map with `std::shared_mutex`. This allows concurrent 'Read' operations (HSS lookups) while providing exclusive 'Write' access for state changes, minimizing latency during bulk attaches."

**Q: "Why use std::atomic instead of a volatile bool for stop flags?"**
*Answer:* "Volatile does not guarantee atomicity or memory ordering. In a multi-threaded system, a data race on a bool can lead to nodes not shutting down or inconsistent states. `std::atomic<bool>` ensures thread-safe access and memory visibility across cores."

---

## 6. COMMAND REFERENCE & TESTING

### EPC Commands (`mme_sim`)
- `MODE [BEGINNER|ENGINEER|INTERVIEW]`: Switch learning depth.
- `CR [N]`: Create N UEs and perform 4G Attach.
- `BULK [N]`: Test thread-pool concurrency.

### IMS Commands (`mme_ims`)
- `REGISTER`: SIP registration flow.
- `CALL`: 2-party VoLTE call with QCI=1 setup.
- `CONF`: 3-party audio bridge setup.

---

## 7. LINKEDIN & PORTFOLIO STRATEGY
- **Video:** Show the "Engineering Mode" log while Wireshark is open. 
- **Proof:** "I didn't just code it; I built the test harness that proves it's 3GPP compliant."

---

## 8. ROADMAP: THE "ELITE" UPGRADES
1. **5G SBA Integration:** AMF (C++) discovery via NRF (Go) over REST.
2. **Zero-Copy Parsing:** Refactor `MessageReader` to use `std::string_view` for zero-allocation parsing of SIP/S1AP strings.
3. **Observability:** Prometheus exporter in the Go NRF to track network health (KPIs like SSR/CSSR).
4. **Security:** Implement 5G SUCI (IMSI Privacy) logic to demonstrate 3GPP Security (TS 33.501) knowledge.

---

## 9. AI COMPARATIVE STRATEGY (QUESTIONS FOR OTHER MODELS)
*If transferring this project to Claude, GPT-4, or other agents, ask these to verify architectural integrity:*
1. **Concurrency Check:** "Our S-CSCF uses `std::shared_mutex` for the call store. Is there a lock-free alternative using atomic pointers that would work better for a 7-year experience portfolio?"
2. **Protocol Resilience:** "How would you implement a 'Chaos Monkey' in our Python automation track to test how the MME handles malformed S1AP packets?"
3. **Cloud Strategy:** "Suggest the most cost-effective AWS architecture to deploy this simulator using ECS Fargate and a Go-based NRF sidecar."

---

## 10. AI HANDOVER (STRICT INSTRUCTIONS)
1. This is a high-performance C++17 telecom project.
2. Prioritize RAII, Atomics, and Exception Safety.
3. Every log must support BEGINNER/ENGINEER/INTERVIEW levels.
4. **Identity:** This is not a "toy" project; it is a learning-focused, carrier-grade simulation architecture.

---

## 11. RECAP OF RECENT IMPROVEMENTS
| Feature | Why? | Senior Level Benefit |
| :--- | :--- | :--- |
| **std::shared_ptr** | Ownership Safety | Eliminates memory leaks in async IMS threads. |
| **shared_mutex** | Lock Contention | Proves understanding of Read-Heavy Telecom workloads. |
| **Custom Exceptions**| Resilience | Demonstrates carrier-grade "Node survival" logic. |
| **Lua Dissector** | Observability | Moves project from "Console logs" to "Industry Standard PA." |
| **GTPv2 Logic** | Tunneling | Shows mastery of UE Data Plane vs Control Plane separation. |