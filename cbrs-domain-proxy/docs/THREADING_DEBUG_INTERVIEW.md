# Threading, Debugging & Production Scenarios
## CBRS → IMS → MTAS — Ericsson Interview Guide

---

## Part 1 — Thread Mental Model (Start Here)

Think of threads like **workers in a call centre**:

```
YOUR LAPTOP (8 cores)
─────────────────────────────────────────────────────────
Core 0    │  Core 1   │  Core 2    │  Core 3   │ (idle)
─────────────────────────────────────────────────────────
main      │  accept   │  cbsd-1    │  cbsd-2   │
(CLI)     │ (waiting  │ (waiting   │ (waiting  │
          │ for conn) │ for msg)   │ for msg)  │
─────────────────────────────────────────────────────────
```

**99% of the time all threads are SLEEPING.**
They block waiting for: a new TCP connection, a message on a socket, or a mutex to unlock.
They use 0% CPU while sleeping. OS wakes them only when work arrives.

### CBRS Domain Proxy — 4 threads

```
Thread 1: main       → reads STATUS/QUIT from your keyboard
Thread 2: accept     → blocked in accept(), waiting for new CBSD connection
Thread 3: cbsd-1     → blocked in recv(), waiting for message from CBSD-1
Thread 4: cbsd-2     → blocked in recv(), waiting for message from CBSD-2
```

### IMS Server — 5 threads

```
Thread 1: main/CLI   → reads commands (STATUS/QUIT)
Thread 2: P-CSCF     → accept(), waiting for UE connections on port 5060
Thread 3: S-CSCF     → recvFrame(), waiting for messages from P-CSCF
Thread 4: IMS-HSS    → accept(), waiting for S-CSCF on port 3870
Thread 5: pcap       → pthread_cond_wait(), idle unless writing
```

**Same pattern.** CBRS and IMS use identical thread design.

---

## Part 2 — What Happens During a Message (e.g. GRANT in CBRS)

```
You type "GRANT" in cbsd_agent
    │
    ▼
cbsd_agent sends JSON over TCP → domain_proxy port 8700
    │
    ▼
Thread 3 (cbsd-1 handler) WAKES UP
[OS: recv() returns data, thread moves SLEEPING → RUNNING]
    │
    ▼
Thread 3 tries to lock g_sas_mtx
  Case A: Thread 4 is already using SAS → Thread 3 SLEEPS (waits for mutex)
  Case B: Free → Thread 3 gets lock, sends to SAS
    │
    ▼
Thread 3 sends GrantRequest to sas_stub → goes back to SLEEPING (waiting for reply)
    │
    ▼
sas_stub thread WAKES UP → processes → sends GrantResponse
    │
    ▼
Thread 3 WAKES UP → reads response → updates registry → sends to cbsd_agent
    │
    ▼
Thread 3 goes back to SLEEPING
Total active CPU time: ~1ms
```

---

## Part 3 — Live Commands to Watch Threads

### Start CBRS and watch

```bash
# Start everything
cd cbrs-domain-proxy/build
./sas_stub &
./domain_proxy &
./cbsd_agent 1 &
./cbsd_agent 2 &

# Watch thread count and CPU
top -pid $(pgrep domain_proxy)
```

**Output (idle):**
```
PID    COMMAND       %CPU  #TH   STATE
48221  domain_proxy   0.0   4    sleeping
```

**While processing GRANT:**
```
PID    COMMAND       %CPU  #TH   STATE
48221  domain_proxy   8.3   4    running   ← brief spike then back to 0
```

### See what each thread is doing right now

```bash
sample $(pgrep domain_proxy) 5
```

**Output while idle:**
```
Thread 48221 (main):     getline()    ← waiting for your input
Thread 48222 (accept):   accept()     ← waiting for connections
Thread 48223 (cbsd-1):   recv()       ← waiting for message
Thread 48224 (cbsd-2):   recv()       ← waiting for message
```

**Output while processing:**
```
Thread 48223 (cbsd-1):
  handleCbsd()
    forwardToSas()
      lock_guard(g_sas_mtx)
        sendMsg()
        recvMsg()    ← brief spike here
```

### Full debugger attach

```bash
lldb -p $(pgrep domain_proxy)
(lldb) thread list              # list all threads
(lldb) thread backtrace all     # where is EVERY thread right now
(lldb) thread select 3          # switch to thread 3
(lldb) bt                       # that thread's call stack
(lldb) detach                   # let process keep running
(lldb) quit
```

---

## Part 4 — Thread Safety: What the Mutexes Protect

### CBRS Domain Proxy

| Shared resource | Type | Protected by | Why |
|---|---|---|---|
| `g_registry` | `unordered_map` | `std::mutex` in CbsdRegistry | Multiple CBSD threads read/write simultaneously |
| `g_sas_sock` | TCP socket | `std::mutex g_sas_mtx` | One stream — interleaving corrupts protocol |
| `g_stop` | Stop flag | `std::atomic<bool>` | Written by CLI thread, read by accept thread |
| Logger | `std::cout` | `std::mutex` inside Logger | Prevents interleaved colored output |

### IMS Simulator

| Shared resource | Protected by | Why |
|---|---|---|
| `call_to_caller_` / `call_to_callee_` | `call_mtx_` | Multiple UE threads routing calls simultaneously |
| `ue_by_impu_` | `ue_mtx_` | Registration state across threads |
| `calls_` in S-CSCF | `calls_mtx_` (shared_mutex) | Concurrent read (STATUS) + write (new call) |
| `call_invite_delivered_` | `call_mtx_` | Re-INVITE guard, multiple threads |

---

## Part 5 — Production System (32 cores vs your 8-core Mac)

```
PRODUCTION ERICSSON MTAS BLADE (32 cores, 128GB RAM)
────────────────────────────────────────────────────────────
Cores 0-1   │ OS + hardware interrupts (never touched by MTAS)
────────────────────────────────────────────────────────────
Cores 2-15  │ SIP SIGNALING PLANE
            │ Thread pool: 200 workers for INVITE/BYE/REGISTER
            │ taskset -c 2-15 ./mtas_process
            │ chrt -f 50 ./mtas_process   (real-time priority)
────────────────────────────────────────────────────────────
Cores 16-23 │ DIAMETER PLANE (Rx/Gx/Cx interfaces)
            │ Thread pool: 50 workers for CCR/CCA
            │ taskset -c 16-23 ./diameter_process
────────────────────────────────────────────────────────────
Cores 24-27 │ MEGACO/H.248 to MRFP (conference mixing)
            │ Dedicated so conference doesn't slow signaling
            │ taskset -c 24-27 ./mrfc_process
────────────────────────────────────────────────────────────
Cores 28-31 │ MANAGEMENT PLANE (O&M, alarms, NETCONF, logging)
            │ Low priority — can starve, calls won't care
────────────────────────────────────────────────────────────
```

**Key commands:**
```bash
# Pin process to specific cores
taskset -c 2-15 ./mtas_process

# Real-time priority — signaling never preempted by OS
sudo chrt -f 50 ./mtas_process

# Reserve cores 0-1 for OS at boot (kernel parameter)
# Add to /etc/default/grub: GRUB_CMDLINE_LINUX="isolcpus=0,1"
```

**Why this matters for interview:**
If a Diameter thread spikes to 100% on core 20, it CANNOT hurt SIP threads on cores 2-15. They are physically isolated. One plane going crazy cannot kill call processing. This is the most important production answer.

---

## Part 6 — Interview Scenario 1: CPU 100% on Diameter Thread

### The question
> "Your MTAS handles 50,000 calls. One Diameter thread hits 100% CPU. Calls start failing. What do you do?"

### Step 1: Find which thread is hot

```bash
top -H -p $(pgrep mtas_process)
```

```
  PID   TID  %CPU  COMMAND
48221  48221   0.2  mtas_process   ← main
48221  48225  99.8  mtas_process   ← THIS ONE ← Diameter worker-3
48221  48226   0.1  mtas_process
48221  48227   0.1  mtas_process
```

TID 48225 is at 99.8%. It's a Diameter worker thread.

### Step 2: Find what function it's stuck in

```bash
sample $(pgrep mtas_process) 10
```

```
Thread 48225 (Diameter-worker-3):    10000 samples out of 10000
  10000 DiameterDecoder::parseAVP()
    10000 while(pos < len) {
      10000   avp = readNextAVP(buf, pos)
      10000   pos += avp.length        ← if avp.length=0, pos NEVER moves!
```

**Found it.** A malformed Diameter message with AVP length=0 causes infinite loop.

### Step 3: Confirm exact line with lldb

```bash
lldb -p $(pgrep mtas_process)
(lldb) thread select 2
(lldb) bt
```

```
frame #0: DiameterDecoder::parseAVP() at diameter.cpp:183   ← exact line
frame #1: DiameterSession::handleCCR() at diameter_session.cpp:67
frame #2: DiameterWorker::processMessage() at diameter_worker.cpp:45
```

### Step 4: The fix

```cpp
// BEFORE — buggy, infinite loop if avp.length == 0:
while (pos < len) {
    AVP avp = readNextAVP(buf, pos);
    pos += avp.length;    // if length=0, pos never moves
}

// AFTER — safe:
while (pos < len) {
    AVP avp = readNextAVP(buf, pos);
    if (avp.length == 0) {
        Logger::warn("Diameter", "Zero-length AVP at pos=" +
                     std::to_string(pos) + " — dropping malformed message");
        break;    // never infinite loop on bad input
    }
    pos += avp.length;
}
```

### Step 5: Production response while fixing

```cpp
// While the fix is being deployed, MTAS sheds load:
if (cpuUsage() > 80) {
    // SIP: reject NEW calls only
    sendSipResponse(503, "Service Unavailable", "Retry-After: 30");

    // Diameter: reject new CCR-Initial
    if (ccr.ccRequestType == INITIAL_REQUEST) {
        sendCCA(DIAMETER_TOO_BUSY);   // response code 3004
    }
    // BUT: still process CCR-Update (active calls) — never drop mid-call
}
```

### Interview answer (say this)
> "First I use `top -H` to find which thread TID is at 100%, then `sample` to get its call stack without stopping the process. In this case it's an infinite loop in the Diameter AVP parser — zero-length AVP means the position counter never advances. Immediate production action: MTAS starts shedding new SIP INVITEs with 503+Retry-After and new Diameter CCR-Initial with code 3004 (TOO_BUSY), while in-flight calls continue. Fix: length guard in the parser. Long term: fuzz the parser with malformed inputs in CI."

---

## Part 7 — Interview Scenario 2: Race Condition on Call Table

### The scenario
> "Two INVITEs arrive for the same callee at the exact same millisecond. Calls get crossed — caller A hears caller C's audio."

### Why it happens

```cpp
// pcscf_node.cpp — WITHOUT mutex (buggy):
void handleInvite(string id, string callee) {
    call_to_callee_[id] = callee;   // Thread 1 writes HERE
}
// Thread 2 reads call_to_callee_[id] AT THE SAME TIME
// Result: Thread 2 reads a half-written value → wrong routing
```

### How to catch it: ThreadSanitizer

```bash
# Build with race detector:
cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
make
./ims_server   # races reported automatically
```

**Output when race happens:**
```
WARNING: ThreadSanitizer: data race
  Write of size 8 by thread T3:
    #0 pcscf_node.cpp:196  call_to_callee_[id] = callee

  Previous read by thread T4:
    #0 pcscf_node.cpp:350  target = call_to_callee_[id]

SUMMARY: Add mutex around call_to_callee_ access
```

### The fix (already in your code)

```cpp
// pcscf_node.cpp — correct version with mutex:
{ std::lock_guard<std::mutex> lk(call_mtx_);
  call_to_callee_[call_id] = to; }     // atomic write, no race
```

---

## Part 8 — Interview Scenario 3: Timer Storm (Heartbeat Thundering Herd)

### The scenario
> "You deploy 500 CBSDs at 9:00 AM. All registered simultaneously. At 9:02:00 AM (heartbeatInterval=120s) all 500 send heartbeats at the same second. SAS hits 100% CPU."

### Why it happens

```
9:00:00 AM  500 CBSDs register
9:02:00 AM  500 heartbeats arrive in the SAME SECOND
9:02:01 AM  500 retry (SAS was slow)
9:02:02 AM  SAS overwhelmed, starts dropping → CBSDs retry harder → death spiral
```

### How to see it

```bash
top -H -p $(pgrep domain_proxy)
```

```
  TID  %CPU  COMMAND
48225  99.9  domain_proxy   ← all threads fighting for g_sas_mtx
48226  99.9  domain_proxy
48227  99.9  domain_proxy
... (500 threads blocked on mutex)
```

### The fix — add random jitter

```cpp
// BEFORE — all heartbeats fire at exactly T+120s:
auto heartbeatTime = registrationTime + 120s;
scheduleHeartbeat(heartbeatTime);

// AFTER — spread over 30-second window:
int jitter = rand() % 30;     // 0-30 seconds random
auto heartbeatTime = registrationTime + 120s + seconds(jitter);
scheduleHeartbeat(heartbeatTime);
// Result: 500 CBSDs now send 500/30 ≈ 17 heartbeats/second — manageable
```

### Add exponential backoff on failure

```cpp
// If SAS is busy or connection fails:
int retryDelay = baseDelay * pow(2, retryCount);  // 1s, 2s, 4s, 8s...
retryDelay += rand() % retryDelay;                // jitter on backoff too
scheduleRetry(retryDelay);
```

---

## Part 9 — Interview Scenario 4: Deadlock (0% CPU, Process Frozen)

### The scenario
> "MTAS is alive (0% CPU, not crashed) but not processing any calls. Calls are queuing up."

### How to diagnose

```bash
# CPU is 0% but process is stuck
lldb -p $(pgrep mtas_process)
(lldb) thread backtrace all
```

**Output:**
```
Thread 3 (SIP-worker-1):
  #0 __lll_lock_wait()
  #1 std::mutex::lock()
  #2 forwardToDiameter()       ← waiting for diameter_mtx

Thread 4 (SIP-worker-2):
  #0 __lll_lock_wait()
  #1 std::mutex::lock()
  #2 forwardToDiameter()       ← also waiting for diameter_mtx

Thread 5 (Diameter-worker):
  #0 recv()
  #1 DiameterSocket::read()
  #2 forwardToSip()            ← HOLDING diameter_mtx, waiting for sip_mtx

Thread 6 (SIP-receiver):
  #0 __lll_lock_wait()
  #1 std::mutex::lock()        ← HOLDING sip_mtx, waiting for another lock
```

**Classic deadlock:** Thread 5 holds `diameter_mtx`, wants `sip_mtx`. Thread 6 holds `sip_mtx`, wants `diameter_mtx`. Both wait forever.

### The fix — always lock mutexes in the same order

```cpp
// WRONG — different order in different threads causes deadlock:
// Thread A: lock(sip_mtx) then lock(diameter_mtx)
// Thread B: lock(diameter_mtx) then lock(sip_mtx)

// RIGHT — always same order everywhere:
std::lock(sip_mtx, diameter_mtx);   // C++17 deadlock-free multi-lock
std::lock_guard<std::mutex> lg1(sip_mtx,      std::adopt_lock);
std::lock_guard<std::mutex> lg2(diameter_mtx, std::adopt_lock);
```

---

## Part 10 — Megaco / H.248: Where It Fits

### What Megaco is

H.248/Megaco is the protocol between:
- **MRFC** (Media Resource Function Controller) — the "brain" deciding what to mix
- **MRFP** (Media Resource Function Processor) — the DSP doing actual audio mixing

```
UE-A types CONF C
    │
    ▼
MTAS receives REFER (add UE-C to conference)
    │ ISC (SIP) — Mr interface
    ▼
MRFC receives SIP INVITE from MTAS
    │ H.248/Megaco — Cr interface — port 2944
    ▼
MRFP: CreateContext — "add 3 RTP streams, mix them"
MRFP: Mix(B+C)→A, Mix(A+C)→B, Mix(A+B)→C   (each hears the other two)
```

This is already in your IMS simulator — see `volte_call_flow.html` Phase 6 (Conference).

### Megaco CPU spike scenario

> "Conference call setup takes 3 seconds instead of 200ms."

```bash
sample $(pgrep mrfc_process) 10
```

```
Thread (H248-worker):
  8000/10000  H248Encoder::buildCreateContext()
    8000  std::string::append()    ← string concat in loop
    8000  malloc/free              ← repeated heap allocation
```

80% of time in string building. Fix:

```cpp
// BEFORE — slow, new allocation every iteration:
std::string msg = "";
for (auto& term : terminations) {
    msg += buildTermination(term);   // allocates new string each time
}

// AFTER — fast, reserve once:
std::string msg;
msg.reserve(terminations.size() * 200);
for (auto& term : terminations) {
    msg.append(buildTermination(term));   // no reallocation
}
```

---

## Part 11 — One-Page MTAS Answer for Ericsson Interview

### "How does MTAS handle high CPU load?"

**Normal (< 50% CPU):**
- SIP INVITE arrives → thread pool picks it up → applies iFC services (OIP, barring, CDR) → forwards → 5ms per call

**Moderate load (50–80% CPU):**
- Threads compete for shared resources (call table mutex, Diameter pool)
- Latency increases: 5ms → 20ms
- Debug: `top -H` to see mutex contention, `lldb thread backtrace all`

**Overload (> 80% CPU):**
- Shed NEW requests:
  - SIP 503 + `Retry-After: 30` for new INVITEs
  - Diameter 3004 (DIAMETER_TOO_BUSY) for new CCR-Initial
- Protect ACTIVE calls — never drop mid-call CCR-Update

**Crisis (100% CPU, one thread spinning):**
1. `top -H` → find which TID is at 100%
2. `sample PID 10` → find which function is looping
3. `lldb -p PID → thread select N → bt` → get exact line
4. Common causes: infinite loop in parser, timer storm, race condition
5. Immediate: `taskset` so runaway can't steal from signaling plane
6. Fix code + add to CI: ThreadSanitizer + parser fuzzing

**Production protection (Ericsson design):**
- SIP plane pinned to cores 2-15 (`taskset`)
- Diameter plane on cores 16-23
- Megaco/H.248 on cores 24-27
- Management on cores 28-31
- Watchdog restarts any plane that stops responding
- Active/standby blade: failover in < 1 second if this MTAS goes crazy

---

## Part 12 — Quick Reference: Commands for the Interview

```bash
# FIND PID
pgrep ims_server
ps -ef | grep ims_server

# WATCH CPU LIVE (all processes)
top
top -pid $(pgrep ims_server)

# SEE ALL THREADS and their CPU
top -H -p $(pgrep ims_server)

# WHAT IS EVERY THREAD DOING RIGHT NOW (no debugger needed)
sample $(pgrep ims_server) 5

# OPEN PORTS AND FILES
lsof -p $(pgrep ims_server) -i

# WATCH CPU OVER TIME (1 second intervals)
while true; do
    ps -p $(pgrep ims_server) -o pid,%cpu,threads,time
    sleep 1
done

# DEEP DEBUG — attach without stopping
lldb -p $(pgrep ims_server)
  (lldb) thread list              # all threads
  (lldb) thread backtrace all     # where every thread is RIGHT NOW
  (lldb) thread select 3          # switch to thread 3
  (lldb) bt                       # call stack
  (lldb) frame variable           # local variables
  (lldb) continue                 # resume
  (lldb) detach                   # disconnect (process keeps running)

# DETECT RACE CONDITIONS (add at build time)
cmake .. -DCMAKE_CXX_FLAGS="-fsanitize=thread -g"
make && ./ims_server    # races reported automatically

# PIN PROCESS TO SPECIFIC CPU CORES (production)
taskset -c 2-7 ./ims_server

# REAL-TIME PRIORITY (call processing never preempted)
sudo chrt -f 50 ./ims_server
```

---

## Connection Map: CBRS → IMS → MTAS → Production

```
CONCEPT              CBRS DOMAIN PROXY         IMS / MTAS
─────────────────────────────────────────────────────────
Shared resource      g_sas_sock (one TCP)       Diameter connection pool
Protected by         g_sas_mtx                  diameter_pool_mtx
                                                 
Call state store     CbsdRegistry               call_to_caller_/callee_ map
Protected by         CbsdRegistry::mu_          call_mtx_
                                                 
State machine        SpectrumState enum          SipDialog state
Enforced in          Domain Proxy (local)        S-CSCF (before SAS/HSS)
                                                 
One-thread-per-conn  Yes (handleCbsd thread)    Yes (per-UE handler thread)
                                                 
Overload response    (not implemented)           SIP 503, Diameter 3004
                                                 
Production scaling   SAS connection pool         Diameter connection pool
                     Thread pool (not done)      Thread pool (in MTAS)
                     Core affinity               Core affinity (real MTAS)
```
