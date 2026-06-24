# CBRS Domain Proxy — C++17 Simulator

A from-scratch simulation of the **CBRS (Citizens Broadband Radio Service)** control plane — implementing the WinnForum SAS-CBSD protocol (WINNF-TS-0016) in C++17 with raw TCP sockets and JSON messaging.

> Part of the [4G + IMS + 5G + CBRS simulator suite](../README.md) — built by a telecom engineer with 8 years of experience in 4G/5G core networks.

---

## What Is CBRS?

CBRS is the **3.5 GHz spectrum band (3550–3700 MHz)** in the US, shared dynamically between:

| Tier | Who | Priority |
|------|-----|----------|
| **Incumbent** | US Navy radar + satellite earth stations | Highest — must be protected |
| **PAL** (Priority Access License) | FCC-licensed operators (paid auction) | High — 10 MHz blocks |
| **GAA** (General Authorized Access) | Anyone with an approved CBSD | Lowest — opportunistic |

The key actors:
- **CBSD** (Citizens Broadband Radio Service Device) = the smart base station (small cell, macro cell, private 5G node)
- **Domain Proxy** = middleware that aggregates multiple CBSDs and manages their SAS communication
- **SAS** (Spectrum Access System) = cloud service (Google, Federated Wireless, CommScope) that coordinates spectrum use, protects incumbents via ESC sensors

---

## Architecture

### System Architecture — Three Processes

```
                 WinnForum CBRS Protocol (WINNF-TS-0016)
                 JSON over TCP (real: HTTPS + mutual TLS)

  ┌─────────────────────────────────────────────────────────────────┐
  │                  CBSD TIER (radio devices)                      │
  │  cbsd_agent 1    cbsd_agent 2    cbsd_agent 3  ...             │
  │  Cat-A 3555MHz   Cat-A 3565MHz   Cat-B 3580MHz                 │
  │  FCC-DEVICE-001  FCC-DEVICE-002  FCC-DEVICE-003                │
  └──────────┬───────────────┬───────────────┬────────────────────┘
             │               │               │  TCP :8700
             ▼               ▼               ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │              DOMAIN PROXY  (this simulator)                     │
  │                                                                 │
  │  Accept thread ──► Thread-1 (CBSD-1 handler)                   │
  │       :8700     ──► Thread-2 (CBSD-2 handler)  ─── g_sas_mtx ─►│
  │                  ──► Thread-3 (CBSD-3 handler)                  │
  │                                                                 │
  │  CbsdRegistry (mutex-protected):                                │
  │    cbsdId → {state, grantId, freq, bw, category}               │
  └──────────────────────────────────────┬──────────────────────────┘
                                         │ TCP :8800 (one connection)
                                         ▼
  ┌─────────────────────────────────────────────────────────────────┐
  │                    SAS STUB  (sas_stub)                         │
  │                                                                 │
  │  handleDpConnection() — one thread per DP connection            │
  │  Registration → assigns cbsdId                                  │
  │  Grant        → assigns grantId, freq, maxEirp                  │
  │  Heartbeat    → refreshes transmit window                       │
  │  Relinquish   → frees spectrum                                  │
  │  Deregister   → removes CBSD record                             │
  └─────────────────────────────────────────────────────────────────┘
```

**What each binary does:**

| Binary | Port | Role |
|--------|------|------|
| `sas_stub` | 8800 | Simulated SAS — grants spectrum, tracks CBSDs |
| `domain_proxy` | 8700 (←CBSDs) / 8800 (→SAS) | Aggregates CBSDs, enforces state machine |
| `cbsd_agent` | (connects to 8700) | Simulated CBSD with interactive CLI |

---

## Multithreading Design

### How the Domain Proxy handles concurrent CBSDs

The Domain Proxy uses a **one-thread-per-CBSD** model:

```cpp
// Accept loop runs in a background thread
std::thread acceptThread([&]() {
    while (!stop.load()) {
        auto client = server.accept();           // blocks waiting for CBSD
        std::thread(handleCbsd,                 // spawn handler thread
                    std::move(client)).detach(); // detach = fire and forget
    }
});
```

Each CBSD connection runs in its own thread (`handleCbsd`). This means:
- **3 CBSDs connected** → 3 handler threads + 1 accept thread + 1 CLI thread = 5 threads total
- Each CBSD thread manages that CBSD's state machine independently
- No CBSD blocks any other CBSD

### Thread-safe shared state

Two shared resources accessed by multiple threads:

**1. CbsdRegistry** — which CBSDs are registered and their grant state:
```cpp
// cbsd_registry.h — guarded by std::mutex
class CbsdRegistry {
    mutable std::mutex mu_;
    std::unordered_map<std::string, CbsdInfo> store_;  // O(1) lookup by cbsdId

    void upsert(const CbsdInfo& info) {
        std::lock_guard<std::mutex> lk(mu_);  // exclusive write
        store_[info.cbsdId] = info;
    }
    bool get(const std::string& cbsdId, CbsdInfo& out) const {
        std::lock_guard<std::mutex> lk(mu_);  // shared read (still exclusive here)
        auto it = store_.find(cbsdId);
        ...
    }
};
```
> **Why `unordered_map`?** O(1) average lookup by cbsdId. A real DP may manage 100+ CBSDs — `map` would give O(log N) per lookup. For a STATUS query scanning all CBSDs, `forEach()` takes a snapshot inside the lock then iterates outside it to minimize lock hold time.

**2. SAS connection** — one TCP socket, serialised with a mutex:
```cpp
static Socket       g_sas_sock;   // shared SAS connection
static std::mutex   g_sas_mtx;    // one CBSD at a time talks to SAS

static std::string forwardToSas(const std::string& req) {
    std::lock_guard<std::mutex> lk(g_sas_mtx);  // serialize SAS access
    sendMsg(g_sas_sock, req);
    std::string resp;
    recvMsg(g_sas_sock, resp);
    return resp;
}
```
> **Why one mutex?** The SAS connection is a single TCP stream — interleaving messages from two CBSD threads would corrupt the protocol. The mutex ensures request+response is atomic per CBSD.

### Thread safety summary

| Resource | Type | Protection | Why |
|----------|------|-----------|-----|
| `CbsdRegistry` | `std::unordered_map` | `std::mutex` | Multiple CBSD threads read/write simultaneously |
| `g_sas_sock` | TCP socket | `std::mutex g_sas_mtx` | One stream — interleaving would corrupt protocol |
| `g_stop` | Stop flag | `std::atomic<bool>` | Written by CLI thread, read by accept thread |
| Logger output | `std::cout` | `std::mutex` inside Logger | Prevents interleaved colored output |

---

## Performance — Current Design and How to Scale

### Current design (good for 1–10 CBSDs)

```
Thread-per-CBSD model:

  CBSD-1 ─► Thread-1 ─► [mutex lock] ─► SAS
  CBSD-2 ─► Thread-2 ─► [wait]         (serialized)
  CBSD-3 ─► Thread-3 ─► [wait]
```

**Bottleneck:** The `g_sas_mtx` serializes all SAS communication. If 50 CBSDs heartbeat simultaneously, they queue up one by one. Heartbeat handling takes ~1ms on localhost — that's 50ms total for 50 CBSDs. Acceptable for a lab simulator.

### How to improve for production scale (interview talking points)

**1. Request batching (WinnForum design intent)**

The real Domain Proxy batches multiple CBSDs' requests into one SAS HTTP call:
```json
// Real WinnForum format — array of requests in one HTTP POST:
{ "registrationRequest": [
    {"fccId": "FCC-001", ...},
    {"fccId": "FCC-002", ...},
    {"fccId": "FCC-003", ...}
  ]
}
// SAS responds with matching array of responses in one HTTP reply
```

Improvement: collect pending requests from all CBSD threads into a queue, flush every 100ms as one batched SAS call. Reduces SAS round-trips from N to 1 for heartbeat storms.

**2. Thread pool instead of thread-per-CBSD**

```cpp
// Current: one OS thread per CBSD (wasteful at 500+ CBSDs)
std::thread(handleCbsd, std::move(client)).detach();

// Better: fixed-size thread pool (e.g. N=16 workers for 500 CBSDs)
ThreadPool pool(16);
pool.enqueue([client = std::move(client)]() { handleCbsd(client); });
```
Each worker picks up the next CBSD connection from the queue. Eliminates context-switch overhead of 500+ threads. The 4G simulator in this repo already implements this pattern (`src/common/thread_pool.h`).

**3. `shared_mutex` for read-heavy registry**

```cpp
// Current: std::mutex (exclusive for both reads and writes)
std::lock_guard<std::mutex> lk(mu_);

// Better: std::shared_mutex (multiple readers concurrently)
mutable std::shared_mutex mu_;

// Read path (STATUS, state check):
std::shared_lock<std::shared_mutex> lk(mu_);  // multiple threads read simultaneously

// Write path (upsert, setState):
std::unique_lock<std::shared_mutex> lk(mu_);  // one writer, all readers blocked
```
When 90% of operations are reads (STATUS queries, state checks before forwarding), `shared_mutex` lets multiple CBSD threads read the registry simultaneously.

**4. Async I/O with `epoll` / `io_uring` (Linux)**

```
Current:  1 thread blocked per CBSD (mostly waiting for SAS response)
Better:   1 event loop handles N CBSDs via non-blocking I/O
```
With `epoll`, a single thread monitors all CBSD sockets. When data arrives on any socket, the thread handles it. Eliminates per-CBSD thread overhead entirely. Used by high-performance DP implementations (nginx-style event loop).

**5. Connection pool to SAS**

```
Current:  1 SAS connection, serialised (bottleneck)
Better:   pool of K SAS connections, round-robin assignment
```
```cpp
// With a pool of 4 SAS connections:
auto& conn = sas_pool[hash(cbsd_id) % 4];  // CBSD always uses same connection
// Eliminates the g_sas_mtx serialization bottleneck
// 4 CBSDs can talk to SAS simultaneously
```

### Performance comparison

| Approach | CBSDs supported | Threads | SAS calls/sec |
|----------|----------------|---------|--------------|
| **Current** (thread-per-CBSD, one SAS conn) | ~50 | N+2 | ~1000 |
| + Thread pool (N=16 workers) | ~500 | 18 | ~1000 |
| + Request batching | ~500 | 18 | ~100 (batched) |
| + `shared_mutex` registry | ~500 | 18 | ~5000 (reads) |
| + Connection pool (K=4) | ~2000 | 18 | ~4000 |
| + `epoll` async I/O | ~10000 | 4–8 | ~10000 |

---

## Build

```bash
cd cbrs-domain-proxy
mkdir -p build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

Produces: `sas_stub`, `domain_proxy`, `cbsd_agent`

---

## Run

**3 terminals, start in this order:**

```bash
# Terminal 1 — SAS (start first)
cd build && ./sas_stub

# Terminal 2 — Domain Proxy
cd build && ./domain_proxy

# Terminal 3 — CBSD Agent (1=Cat-A 3555MHz, 2=Cat-A 3565MHz, 3=Cat-B 3580MHz)
cd build && ./cbsd_agent 1
```

**LOG_LEVEL** controls verbosity:

| Value | Output |
|-------|--------|
| `ENGINEER` (default) | JSON messages, state transitions, field details |
| `BEGINNER` | Plain-English story ("CBSD is now transmitting") |
| `INTERVIEW_T` | CBRS spec details, FCC rules, ESC sensors, PAL/GAA |
| `ALL` | Everything |

```bash
LOG_LEVEL=ALL ./sas_stub   # shows all log levels simultaneously
```

---

## CBSD Commands (type in `cbsd_agent` terminal)

| Command | What happens |
|---------|-------------|
| `REGISTER` | Sends RegistrationRequest (FCC ID, location, antenna info) → SAS assigns cbsdId |
| `GRANT` | Sends GrantRequest (desired freq + BW) → SAS assigns grantId + channel |
| `HEARTBEAT` | Sends HeartbeatRequest → refreshes transmit window → state = AUTHORIZED |
| `RELINQUISH` | Voluntarily returns spectrum grant → state = REGISTERED |
| `DEREGISTER` | Removes CBSD from SAS → state = UNREGISTERED |
| `STATUS` | Show current state, cbsdId, grant details |
| `QUIT` | Disconnect |

**Domain Proxy commands** (type in `domain_proxy` terminal):

| Command | What it does |
|---------|-------------|
| `STATUS` | Show all registered CBSDs and their states |
| `QUIT` | Shutdown |

---

## Complete Flow (what you see end-to-end)

```
Step 1 — REGISTER:
  CBSD → DP → SAS: {"type":"RegistrationRequest","fccId":"FCC-DEVICE-001",...}
  SAS → DP → CBSD: {"type":"RegistrationResponse","cbsdId":"CBSD-SAS-1","responseCode":0}
  State: UNREGISTERED → REGISTERED

Step 2 — GRANT:
  CBSD → DP → SAS: {"type":"GrantRequest","cbsdId":"CBSD-SAS-1","operationFrequencyMHz":3555,...}
  SAS → DP → CBSD: {"type":"GrantResponse","grantId":"GRANT-1","channelType":"GAA",...}
  State: REGISTERED → GRANTED

Step 3 — HEARTBEAT:
  CBSD → DP → SAS: {"type":"HeartbeatRequest","grantId":"GRANT-1","operationState":"GRANTED"}
  SAS → DP → CBSD: {"type":"HeartbeatResponse","heartbeatInterval":120,...}
  State: GRANTED → AUTHORIZED  ← CBSD is now transmitting on 3555 MHz

Step 4 — RELINQUISH:
  CBSD → DP → SAS: {"type":"RelinquishmentRequest","grantId":"GRANT-1"}
  State: AUTHORIZED → REGISTERED

Step 5 — DEREGISTER:
  CBSD → DP → SAS: {"type":"DeregistrationRequest","cbsdId":"CBSD-SAS-1"}
  State: REGISTERED → UNREGISTERED
```

---

## Spectrum Allocation State Machine

```
UNREGISTERED
    │  RegistrationRequest → RegistrationResponse (responseCode=0)
    ▼
REGISTERED      ← can request spectrum
    │  GrantRequest → GrantResponse (grantId assigned)
    ▼
GRANTED         ← has channel, not yet transmitting
    │  HeartbeatRequest → HeartbeatResponse (transmit window opens)
    ▼
AUTHORIZED      ← actively transmitting on granted channel
    │  RelinquishmentRequest → RelinquishmentResponse
    ▼
REGISTERED      ← spectrum freed, can request new grant
    │  DeregistrationRequest → DeregistrationResponse
    ▼
UNREGISTERED
```

The **Domain Proxy enforces** this state machine locally before forwarding to SAS — rejects out-of-order messages (e.g., GrantRequest before registration) without a cloud round-trip.

---

## Connection to 4G/5G

CBRS is the **spectrum layer** that private 5G networks (O-RAN, LTE-in-a-box) run on:

```
Private 5G on CBRS:
  UE (phone) ──NR Uu──► CBSD (gNB) ──N2/S1──► Core (AMF/MME)
                             │
                    (CBRS SAS protocol)
                             │
                         Domain Proxy
                             │
                            SAS (spectrum coordinator)

  The CBSD is simultaneously:
    - A 5G gNB serving UEs (like the gNB in this repo's 5g-simulator)
    - A CBRS client getting spectrum authorization from SAS
```

In a real private 5G deployment:
- The `gNB` in `5g-simulator/` is the radio function
- The `cbsd_agent` here manages that gNB's **right to transmit** on 3.5 GHz spectrum
- Without a valid SAS grant, the gNB cannot transmit (FCC Part 96 rule)

---

## SAS Response Codes (WinnForum WINNF-TS-0016 Table 14)

| Code | Meaning | When you see it |
|------|---------|----------------|
| 0 | SUCCESS | Normal operation |
| 100 | VERSION_MISMATCH | Wrong protocol version |
| 102 | MISSING_PARAM | Required field absent in request |
| 103 | INVALID_VALUE | Field value out of range |
| 105 | DEREGISTER | SAS-initiated deregistration (ESC event) |
| 300 | UNSUPPORTED_SPECTRUM | Requested channel unavailable |
| 302 | GRANT_CONFLICT | Another CBSD already has this channel |
| 400 | TERMINATED_GRANT | Grant revoked by SAS (missed heartbeat / ESC) |
| 401 | SUSPENDED_GRANT | Temporarily suspended (PAL priority) |

---

## Project Structure

```
cbrs-domain-proxy/
├── src/
│   ├── common/
│   │   ├── logger.h          Color-coded logger (LOG_LEVEL env var)
│   │   ├── socket_wrapper.h  RAII TCP socket (same pattern as 4G/5G sims)
│   │   └── wire.h            JSON framing + flat JSON builder/parser
│   ├── proxy/
│   │   ├── spectrum_state.h  State machine enum + WinnForum response codes
│   │   ├── cbsd_registry.h   Thread-safe CBSD store (mutex + unordered_map)
│   │   └── domain_proxy.cpp  ★ Domain Proxy main (accept loop, SAS forward)
│   ├── sas/
│   │   └── sas_stub.cpp      ★ Simulated SAS (grants, heartbeats, deregistration)
│   └── cbsd/
│       └── cbsd_agent.cpp    ★ CBSD simulator CLI (all 5 protocol steps)
├── docs/
│   └── CBRS_INTERVIEW_GUIDE.md  Interview prep: CBRS concepts + Q&A
└── CMakeLists.txt
```

---

## Related Standards

| Standard | What it covers |
|----------|---------------|
| WINNF-TS-0016 | WinnForum SAS-CBSD Interface Specification (the protocol this sim implements) |
| FCC Part 96 | CBRS rules: power limits, CBSD categories, SAS certification requirements |
| FCC Part 96.41 | EIRP limits: Cat-A ≤30 dBm, Cat-B ≤47 dBm |
| 3GPP TS 38.104 | NR operating bands — Band n48 = 3550-3700 MHz (CBRS band) |
| O-RAN WG4 | Fronthaul interface between O-RU (radio) and O-DU (baseband) |
