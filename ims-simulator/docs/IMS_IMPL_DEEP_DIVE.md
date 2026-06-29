# IMS Simulator — Implementation Deep Dive
> Interview reference: threads, state machines, synchronization, encoder/decoder,
> subscriber database, sockets, memory, scaling, failure modes.
> Senior system design Q&A: HA, scaling, throttling, stateful vs stateless.

---

## 1. The Big Picture — What Runs and Where

```
One process. Four nodes. All talking over TCP on localhost.

[main thread]
    │
    ├── hss_th    → ImsHssNode     port 3870   Cx interface (Diameter)
    ├── scscf_th  → ScscfNode      port 5070   SIP registrar + MTAS logic
    └── pcscf_th  → PcscfNode      port 5060   SIP proxy for UEs
                       ├── scscf_rx_th          watches S-CSCF socket, routes to UEs
                       ├── ue_thread[0]         handles UE-A forever
                       ├── ue_thread[1]         handles UE-B forever
                       └── ue_thread[2]         handles UE-C forever
```

**Thread count formula:**

| UEs connected | Total threads |
|---|---|
| 0 | 5 (main + hss + scscf + pcscf/accept + scscf_rx) |
| 3 | 8 |
| 10 | 15 |
| N | 5 + N |

---

## 2. Every Thread — What It Is, Why It Exists, What It Does

---

### Thread 1 — `main`
**Created by:** the OS when the process starts.

**What it does:**
```
reads your commands from the keyboard
    REG A     → calls doRegister("A")
    CALL A B  → calls doCall("A","B")
    CONF      → calls doConference()
    WAIT      → calls doCallWait()
    BARR      → calls doBarring()
    BYE       → calls doBye()
    STATUS    → calls doStatus()
```

**Why it exists:** Someone has to drive the flow. The main thread is the
conductor — it sequences the SIP messages, writes to PCAP, logs steps.

**Why it cannot also run the servers:** Because `getline()` blocks waiting
for keyboard input. If main also ran the HSS TCP server, it could not read
your keyboard at the same time.

---

### Thread 2 — `hss_th`  (ImsHssNode)
**Spawned:** `std::thread hss_th([&]{ ims_hss.run(); });`
**Port:** 3870 TCP

**What it does:**
```
run()
  └── setupServer()    bind 3870, wait for S-CSCF to connect
                       set ims_hss_ready_ = true
  └── receiveLoop()    loop forever:
                           recv() from S-CSCF TCP socket
                           if SAR → handleSAR() → send SAA back
```

**Why it exists:** During REGISTER, S-CSCF asks HSS "is this subscriber
known? give me their service profile." That is a synchronous request that
must always be available. If HSS ran on the main thread it would block
everything else.

**What the HSS stores (subscriber database):**
```
In our sim: hardcoded in-memory map (compiled in)
In production: Oracle DB / Cassandra / custom HLR

Per subscriber:
  IMPU  → sip:+919000000001@ims.domain   (public identity)
  IMPI  → 919000000001@ims.mnc010.mcc404  (private identity for auth)
  Ki    → secret key for IMS-AKA
  iFC   → Initial Filter Criteria (which AS/MTAS to trigger and when)
  MSISDN, service profile, barring flags
```

**SAR/SAA flow:**
```
S-CSCF sends SAR (TLV):
  CX_IMPU = "sip:+919000000001@ims.domain"
  CX_IMPI = "919000000001@ims.mnc..."

HSS sends SAA back (TLV):
  subscriber profile, iFC rules, MSISDN
  S-CSCF uses iFC to know: "invoke MTAS for every INVITE from this subscriber"
```

---

### Thread 3 — `scscf_th`  (ScscfNode)
**Spawned:** `std::thread scscf_th([&]{ scscf.run(); });`
**Port:** 5070 TCP

**What it does:**
```
run()
  └── setupServer()
        bind 5070
        connect to HSS:3870   (Cx link)
        wait for P-CSCF to connect on 5070
        set scscf_ready_ = true
  └── receiveLoop()
        loop forever:
          recv() from P-CSCF TCP socket
          read 2-byte message type from TLV header
          switch(type):
            SIP_REGISTER → handleRegister()
            SIP_INVITE   → handleInvite()
            SIP_ACK      → handleAck()
            SIP_200_OK   → handle200Ok()
            SIP_BYE      → handleBye()
```

**Why single-threaded:** P-CSCF sends messages one at a time over one TCP
connection. S-CSCF never needs to handle two P-CSCF messages simultaneously.
One loop is enough.

**What it stores — subscriber registry:**
```cpp
std::map<std::string, ImsSubscriber> registry_;
mutable std::shared_mutex            registry_mtx_;

struct ImsSubscriber {
    std::string impu;       // sip:+919000000001@ims.domain
    std::string impi;       // 919000000001@ims.mnc010.mcc404
    std::string contact;    // sip:ue@10.0.0.1:5060  ← 4G P-GW IP from EPC!
    std::string scscf_name; // sip:scscf.ims.domain
    bool        registered;
};
```

**Why `shared_mutex` for the registry:**
```
Registration (WRITE) → rare, happens once per UE
INVITE routing (READ) → frequent, every call reads registry
shared_mutex: many concurrent reads OK, write gets exclusive access
```

**What it stores for active calls:**
```cpp
std::map<std::string, CallState> calls_;
mutable std::shared_mutex        calls_mtx_;

struct CallState {
    std::string caller_impu;
    std::string callee_impu;
    std::string call_id;
    bool        on_hold;
    bool        in_conference;
};
// inserted on INVITE, erased on BYE
```

**MTAS logic lives here too:**
```cpp
// Inside handleInvite():
bool invokeMtas(caller, callee, call_id, sdp) {
    if (MtasState::isBarred(caller))  return false; // OIB barring
    // check iFC triggers from subscriber profile
    // apply call forwarding if configured
    // check call waiting if callee is already in-call
    return true; // "continue routing"
}
```

---

### Thread 4 — `pcscf_th` / becomes the `acceptLoop`
**Spawned:** `std::thread pcscf_th([&]{ pcscf.run(); });`
**Port:** 5060 TCP

When `pcscf.run()` is called, it:
1. Connects to S-CSCF:5070 (outbound TCP)
2. Binds port 5060 (inbound for UEs)
3. Spawns Thread 5 (`scscf_rx_th`)
4. **Becomes** the accept loop itself — sits forever in `acceptLoop()`

```cpp
void PcscfNode::run() {
    connectToSCscf();                             // connect out to S-CSCF
    server_socket_ = createServer(5060);          // bind for UEs
    pcscf_ready_.store(true);

    std::thread scscf_th([this]{ scscfReceiveLoop(); }); // Thread 5
    acceptLoop();      // THIS thread IS the accept loop from here
    scscf_th.join();
    for (auto& t : ue_threads_) t.join();
}

void PcscfNode::acceptLoop() {
    while (!stop_) {
        Socket ue_sock = server_socket_.accept(); // BLOCKS until UE connects
        auto ses = std::make_shared<UeSession>(); // shared ownership — see memory section
        ses->sock = std::move(ue_sock);
        { lock_guard lk(ue_mtx_); ue_sessions_.push_back(ses); }
        ue_threads_.emplace_back([this, ses]{ ueReceiveLoop(ses.get()); });
        // ↑ spawns Thread 6, 7, 8 … one per UE
    }
}
```

**Why `shared_ptr<UeSession>` here:**
```
The session object is owned by two things:
  1. ue_sessions_ vector  (for STATUS display and cleanup at shutdown)
  2. The ue_thread lambda  (for receiving messages from that UE)

Raw pointer: session could be erased from ue_sessions_ while
             ue_thread still uses it → use-after-free crash.

shared_ptr: object stays alive until BOTH owners release it.
  ue_thread exits → ref count: 2 → 1 (not deleted yet)
  ue_sessions_.clear() at shutdown → ref count: 1 → 0 → deleted safely
```

---

### Thread 5 — `scscf_rx_th`  (inside PcscfNode)
**Spawned:** inside `pcscf.run()` just before `acceptLoop()`

**What it does:**
```
scscfReceiveLoop()
  loop forever:
    recv() from S-CSCF TCP socket
    handleFromScscf(payload)
      read From/To/Call-ID TLV fields
      if response (180 Ringing, 200 OK):
        look up caller IMPU in call_to_caller_ map
        → find caller's UE socket in ue_by_impu_
        → forward response to caller's UE
      if request to callee (INVITE):
        look up callee IMPU in ue_by_impu_
        → forward INVITE to callee's UE socket
```

**Why this thread exists separately from Thread 4:**
Thread 4 is stuck blocking in `accept()` waiting for new UE connections.
It cannot simultaneously watch the S-CSCF socket.
Thread 5 watches ONLY the S-CSCF socket and routes everything back to the right UE.

**The routing tables it uses:**
```cpp
// protected by call_mtx_
std::map<std::string, std::string> call_to_caller_; // call_id → caller IMPU
std::map<std::string, std::string> call_to_callee_; // call_id → callee IMPU

// protected by ue_mtx_
std::map<std::string, UeSession*>  ue_by_impu_;    // IMPU → UE socket ptr
```

---

### Threads 6, 7, 8 — `ue_thread[0]`, `[1]`, `[2]`
**Spawned:** by `acceptLoop()` when each UE connects

**What each does:**
```
ueReceiveLoop(ses)
  loop forever:
    recv() from THIS UE's TCP socket  ← BLOCKS until UE sends something
    handleFromUe(ses, payload)
      read message type from TLV
      if REGISTER:
        store IMPU → socket mapping in ue_by_impu_    (write, needs ue_mtx_)
      if INVITE:
        record call_to_caller_, call_to_callee_        (write, needs call_mtx_)
        forward TLV frame to S-CSCF
      if ACK / BYE:
        forward to S-CSCF
```

**Why one thread per UE:**
`recv()` blocks. UE-A can be silent for 30 seconds while UE-B sends
an INVITE. If one thread handled all three UEs, silence from UE-A would
freeze UE-B and UE-C too. One thread per UE = simple blocking I/O.

---

## 3. Synchronization — Every Mutex and Why

```
Object          Type              Protects                  Threads that use it
──────────────  ────────────────  ────────────────────────  ─────────────────────
ue_mtx_         std::mutex        ue_sessions_              Thread 4 (writes on connect)
                                  ue_by_impu_               Threads 6/7/8 (write REGISTER)
                                                            Thread 5 (reads for routing)
                                                            main (reads for STATUS)

call_mtx_       std::mutex        call_to_caller_           Threads 6/7/8 (write on INVITE)
                                  call_to_callee_           Thread 5 (reads for routing)

registry_mtx_   std::shared_mutex registry_ in S-CSCF      scscf_th (writes on REGISTER)
                                                            scscf_th (reads on INVITE)
                                                            (single thread but future-safe)

calls_mtx_      std::shared_mutex calls_ in S-CSCF         scscf_th only (single thread)

MtasState::mtx_ std::mutex        barred set                scscf_th (reads barring)
                                                            ims_server_main CLI (writes)

stop_           std::atomic<bool> shutdown flag             all threads read it
                                                            main thread writes it

*_ready_        std::atomic<bool> startup coordination      see startup section
```

**Why two separate mutexes in P-CSCF (ue_mtx_ and call_mtx_)?**

If both registrations and call routing shared one mutex:
- UE-A registers (writes ue_by_impu_) — LOCK
- Meanwhile UE-B's INVITE arrives (wants to write call_to_caller_) — BLOCKED

Separate mutexes = separate operations never block each other.
Registration and call setup run concurrently.

---

## 4. Startup Coordination — Why Order Matters

```
Time 0:   hss_th starts  → binds 3870 → sets ims_hss_ready_ = true

Time ~0:  scscf_th starts → SPINS on ims_hss_ready_ == false
                          → ims_hss_ready_ becomes true
                          → connects to HSS:3870 (Cx link UP)
                          → binds 5070 → sets scscf_ready_ = true

Time ~0:  pcscf_th starts → SPINS on scscf_ready_ == false
                          → connects to S-CSCF:5070
                          → binds 5060 → sets pcscf_ready_ = true

Time 600ms: main thread sleeps 600ms to let nodes start
            then connects to P-CSCF:5060 as "UE simulator"
```

**Why `atomic<bool>` flags and not `sleep()`:**
`sleep(500ms)` is a guess. On a slow machine HSS may not be ready in 500ms.
The atomic flag is exact — S-CSCF spins checking the flag until HSS actually
sets it. The spin loop also checks `stop_` so QUIT works cleanly.

```cpp
// S-CSCF startup — waits for HSS
for (int i = 0; i < 50 && !stop_.load(); ++i) {
    try { hss_conn_ = Socket::connectTo(HSS_IP, 3870); break; }
    catch (...) { std::this_thread::sleep_for(100ms); }
}
// Not a busy spin — 100ms sleep between attempts
```

---

## 5. Encoder and Decoder — TLV Binary Protocol

All inter-node messages are **TLV frames** (Type-Length-Value):

```
Wire format:
[4B total length][2B message type][2B IE count]
[2B tag][2B length][N bytes value]   ← repeated
[2B tag][2B length][N bytes value]
...
```

**Message types (the 2-byte type field):**
```cpp
enum class SipMsgType : uint16_t {
    SIP_REGISTER    = 0x0501,   // REGISTER request
    SIP_INVITE      = 0x0502,   // INVITE request
    SIP_ACK         = 0x0503,
    SIP_BYE         = 0x0504,
    SIP_100_TRYING  = 0x0510,
    SIP_180_RINGING = 0x0511,
    SIP_200_OK      = 0x0513,
    DIA_CX_SAR      = 0x0603,   // S-CSCF → HSS: Server-Assignment-Request
    DIA_CX_SAA      = 0x0604,   // HSS → S-CSCF: Server-Assignment-Answer
    DIA_RX_AAR      = 0x0701,   // P-CSCF → PCRF: AA-Request (triggers QCI=1)
};
```

**IE tags (fields inside each message):**
```cpp
enum class SipTag : uint16_t {
    SIP_FROM    = 0x0500,   // "sip:+919000000001@ims.domain"
    SIP_TO      = 0x0501,   // "sip:+919000000002@ims.domain"
    SIP_CONTACT = 0x0502,   // "sip:ue@10.0.0.1:5060"  ← 4G IP!
    SIP_CALL_ID = 0x0503,   // unique per dialog
    SIP_CSEQ    = 0x0504,
    SIP_SDP     = 0x0505,   // media description
    CX_IMPU     = 0x0600,
    CX_IMPI     = 0x0601,
};
```

**Decoding — MessageReader:**
```cpp
std::vector<uint8_t> payload = recvFrame();   // exactly N bytes
MessageReader r(payload);
auto type = static_cast<SipMsgType>(r.msgType());

while (r.hasMore()) {
    Tag tag; uint16_t len;
    r.peek(tag, len);
    if (tag == SipTag::SIP_FROM)
        std::string from = r.readStr();
    else if (tag == SipTag::SIP_CALL_ID)
        std::string cid = r.readStr();
    else
        r.skip();    // unknown IE — skip safely (forward compatibility)
}
```

**Encoding — MessageWriter:**
```cpp
MessageWriter w(SipMsgType::SIP_INVITE);
w.addStr(SipTag::SIP_FROM,    caller_impu);
w.addStr(SipTag::SIP_TO,      callee_impu);
w.addStr(SipTag::SIP_CALL_ID, call_id);
w.addStr(SipTag::SIP_SDP,     sdp_offer);
auto frame = w.build();           // returns vector<uint8_t> with length prefix
sock.sendFrame(frame);
```

**Why TLV and not real SIP text between nodes:**
Real SIP is text over UDP/TCP — parsing it requires `find()`, `substr()`,
header continuation folding, quoted-string handling. TLV is fixed binary:
read tag → read length → read exactly N bytes. Fast, no ambiguity.

**The real SIP text** is generated by `SipText::buildRegister()`,
`SipText::buildInvite()` etc. and written to `ims_capture.pcap` so
Wireshark shows the correct SIP flow. Nodes talk TLV internally; the
PCAP shows what a real network trace looks like.

---

## 6. State Machines

### UE State (tracked in `g_ues` map, main thread)

```
UNREGISTERED
    │  REG command → doRegister() → SIP REGISTER sent
    ▼
REGISTERING
    │  200 OK received → ue.registered = true
    ▼
REGISTERED
    │  CALL command → doCall() → SIP INVITE sent
    ▼
CALLING       (INVITE out, waiting for callee)
    │  180 Ringing received
    ▼
ALERTING      (callee phone ringing, caller hears ringback)
    │  200 OK (callee answered)
    ▼
IN-CALL  ←──────────────────────────────────────────┐
    │                                                │
    ├── re-INVITE (hold)  →  ON-HOLD                │
    │        └── re-INVITE (resume) ────────────────┘
    │
    │  BYE → doBye()
    ▼
REGISTERED    (back to registered, call cleaned up)
```

### Bearer State (the 4G EPC side)

```
DEFAULT bearer only  (QCI=9 — normal data, best effort)
    │  ACK sent + Diameter Rx AAR to PCRF
    ▼
DEFAULT (QCI=9)  +  DEDICATED (QCI=1)   ← voice active on QCI=1
    │  BYE + Diameter Rx STR to PCRF
    ▼
DEFAULT bearer only  (QCI=9)             ← QCI=1 bearer released
```

### Call State (inside S-CSCF, per call_id)

```
inserted in calls_   when INVITE arrives
  on_hold = false
  in_conference = false

on_hold = true       when re-INVITE with a=inactive SDP
on_hold = false      when re-INVITE with a=sendrecv SDP

in_conference = true when CONF command invokes MRFC

erased from calls_   when BYE arrives
```

### MTAS State (barring, in MtasState namespace)

```
std::set<std::string> barred;   // set of barred IMPUs

BARR A command  → barred.insert(IMPU_A)
UNBARR A        → barred.erase(IMPU_A)
on INVITE       → if barred.count(caller_impu) → return 603 Decline
```

---

## 7. Subscriber Database

### Our implementation (in-memory)

```cpp
// Inside ScscfNode
std::map<std::string, ImsSubscriber> registry_;

struct ImsSubscriber {
    std::string impu;       // sip:+919000000001@ims.domain
    std::string impi;       // 919000000001@ims.mnc010.mcc404
    std::string contact;    // sip:ue@10.0.0.1:5060  ← 4G IP from EPC attach
    std::string scscf_name; // which S-CSCF serves this subscriber
    bool        registered;
};
```

Written on REGISTER, read on every INVITE.
Lost on restart (no persistence). Enough for 3 UEs.

### What a real telecom subscriber database looks like

```
Legacy operators:    HLR (Home Location Register) — custom proprietary DB
3G/4G IMS:          Oracle DB — used by Ericsson, Nokia HSS products
Cloud-native 5G:     Cassandra — distributed, no single point of failure
Session cache:       Redis — fast in-memory for active session state

Key data per subscriber:
  IMPU list          multiple public identities per subscriber
  IMPI               private identity for auth
  Ki                 secret key for IMS-AKA (never leaves HSS)
  iFC                Initial Filter Criteria — which AS/MTAS to invoke
  service profile    call waiting, forwarding, barring flags
  MSISDN             +919000000001
  S-CSCF assignment  which S-CSCF currently registered for this IMPU

Query pattern:
  Lookup by IMPU     primary key — O(1) in Cassandra, O(log n) in std::map
  Write on register  infrequent (once per UE per session)
  Read on every call frequent (every INVITE does a registry lookup)
```

---

## 8. Socket Model

```
Node         Role            Port    Protocol    Frame
──────────── ─────────────── ─────   ─────────── ────────────────────────
IMS-HSS      TCP server      3870    TCP         4-byte length + TLV body
S-CSCF       TCP server      5070    TCP         4-byte length + TLV body
P-CSCF       TCP server      5060    TCP         4-byte length + TLV body
main/UE-sim  TCP client      →5060   TCP         same framing

Connections:
  S-CSCF    →  IMS-HSS  :3870   (Cx link — Diameter SAR/SAA)
  P-CSCF    →  S-CSCF   :5070   (SIP forwarding)
  main      →  P-CSCF   :5060   (UE simulator sends REGISTER/INVITE)
```

**Socket RAII — why it matters:**
```cpp
class Socket {
    int fd_{-1};
public:
    ~Socket() {
        if (fd_ >= 0) ::close(fd_);   // always closed, no fd leak
    }
    Socket(Socket&& o) noexcept : fd_(o.fd_) {
        o.fd_ = -1;   // move: old socket no longer owns fd
    }
    Socket(const Socket&) = delete;   // no accidental copy → no double-close
};
```

If `handleRegister()` throws an exception, the local Socket goes out of scope
and the destructor closes the fd automatically. No `close(fd)` needed in
every catch block.

**Framing — why the 4-byte length prefix:**
TCP is a stream — recv() may return 50 bytes of a 200-byte message.
The 4-byte length prefix tells the reader exactly how many bytes to wait for
before attempting to parse. Without it: partial parse, corruption.

```cpp
bool recvFrame(Socket& s, std::vector<uint8_t>& out) {
    uint8_t hdr[4];
    if (!recvExact(s, hdr, 4)) return false;
    uint32_t len = (hdr[0]<<24)|(hdr[1]<<16)|(hdr[2]<<8)|hdr[3];
    out.resize(len);
    return recvExact(s, out.data(), len);   // blocks until ALL bytes arrive
}
```

---

## 9. Memory Management

```
Object                How managed              Why
────────────────────  ───────────────────────  ──────────────────────────────
UeSession             std::shared_ptr          owned by ue_sessions_ AND
                                               by ue_thread lambda
                                               deleted only when both release

Socket                RAII (destructor)        fd closed even on exception

vector<uint8_t>       per-message, local       each thread allocates its own
payload buffers       automatic (stack/heap)   no sharing needed

registry_ map         inside ScscfNode         lives for process lifetime
                      (not heap-allocated)     no smart pointer needed

calls_ map            inside ScscfNode         entries inserted on INVITE,
                                               erased on BYE
```

**The `shared_ptr<UeSession>` pattern in detail:**
```cpp
// In acceptLoop():
auto ses = std::make_shared<UeSession>();
ses->sock = std::move(ue_sock);

{
    lock_guard lk(ue_mtx_);
    ue_sessions_.push_back(ses);         // ref count = 2
}
ue_threads_.emplace_back([this, ses]{    // ses captured by value = ref count = 3
    ueReceiveLoop(ses.get());
});                                       // lambda holds it while thread runs

// When UE disconnects:
// ueReceiveLoop returns → lambda destroyed → ref count = 2
// At shutdown:
// ue_sessions_.clear() → ref count = 1 → 0 → UeSession deleted
```

---

## 10. When It Breaks — Failure Modes

**1. S-CSCF loses HSS connection mid-call**
```
In-flight INVITE needs Cx SAR/SAA.
sendCxSAR() sends but recvFrame() on hss_conn_ returns false.
Current: throws exception, caught in receiveLoop, S-CSCF exits.
Fix: reconnect loop (same as P-CSCF → S-CSCF reconnect already has).
```

**2. Two UEs register with the same IMPU simultaneously**
```
ue_thread[0] and ue_thread[1] both do handleFromUe → write ue_by_impu_[impu]
ue_mtx_ serialises this — second write overwrites first.
Real IMS: S-CSCF deregisters old contact, registers new one. Same behaviour.
```

**3. P-CSCF loses S-CSCF connection**
```cpp
// scscfReceiveLoop already handles this:
if (!scscf_conn_.recvFrame(payload)) {
    Logger::warn("P-CSCF", "S-CSCF lost — reconnecting");
    for (int i = 0; i < 30 && !stop_; ++i) {
        sleep(200ms);
        try { scscf_conn_ = connectTo(SCSCF_IP, 5070); break; }
        catch (...) {}
    }
}
// 6 second retry window before giving up
```

**4. UE disconnects while its ue_thread is in handleFromUe**
```
recvFrame() returns false → ueReceiveLoop exits.
The shared_ptr ref count drops (lambda destroyed) but UeSession still alive
in ue_sessions_. No crash. Session cleaned up at shutdown.
On a real system: S-CSCF should receive a SIP REGISTER with Expires=0
(deregistration) or a network-level timeout would trigger it.
```

**5. MTAS barring check is not thread-safe between reads and writes**
```
scscf_th reads MtasState::isBarred() during handleInvite
CLI thread writes MtasState::setBarred() via BARR command
MtasState::mtx() protects both — no race condition
```

**6. Too many UEs — thread exhaustion**
```
Linux default: ~1024 threads per process (ulimit -u)
At 1000 UEs: 1005 threads → hitting limit
Fix: epoll-based single-threaded event loop instead of one-thread-per-UE
     One thread handles all UE sockets with non-blocking I/O
     Scales to 100k+ connections on one core (nginx/node.js model)
```

---

## 11. How to Scale This to Production

### Scale 1 — More UEs on same machine
```
Current:  3 UEs, 8 threads
10 UEs:   15 threads  — works fine
100 UEs:  105 threads — still fine
1000 UEs: epoll needed (replace per-UE thread with event loop)
```

### Scale 2 — Separate processes / machines
```
Current:  1 process, localhost TCP
Prod:     Each node is a separate process/container

IMS-HSS  → own server, Oracle / Cassandra backend
           expose TCP:3870 (Cx), TCP:3868 (Sh for MTAS)
S-CSCF   → multiple instances, load balanced by I-CSCF
           each handles a slice of subscribers (by IMPU hash)
P-CSCF   → one per Radio Access site / geographic cluster
           all connect to S-CSCF pool
```

### Scale 3 — Real subscriber database
```
Our:   std::map in RAM, 3 subscribers, lost on restart

Prod migration path:
  Step 1: PostgreSQL — relational, ACID, good for subscriber profiles
  Step 2: Redis cache — cache active sessions for sub-millisecond lookup
  Step 3: Cassandra — when you need multiple data-centre replication
          Operator example: Vodafone India uses distributed HSS (cloud HSS)
          with Cassandra backend for VoLTE subscriber data

Key query patterns:
  Lookup IMPU      → primary key, O(1) hash or O(log n) B-tree
  Lookup MSISDN    → secondary index (MSISDN maps to IMPU)
  Update contact   → on every REGISTER, must be fast
  Read iFC rules   → on every call setup, must be cached
```

### Scale 4 — Real protocols
```
Our:   TLV binary over TCP (simplified)
Prod:  SIP text over UDP/TCP/TLS (port 5060/5061)
       Diameter over SCTP (port 3868)
       SCTP: multi-stream, no head-of-line blocking, built-in failover
       Each Diameter session has its own stream in the SCTP association
```

---

## 12. Interview — SAY THIS

**"Walk me through the IMS simulator architecture."**

> "It is one process with 8 threads when 3 UEs are connected. The main
> thread drives the CLI — REGISTER, CALL, BYE. Three server nodes run as
> threads: IMS-HSS on port 3870 handling Diameter Cx SAR/SAA, S-CSCF on
> 5070 handling SIP routing and MTAS service logic, P-CSCF on 5060 as the
> UE-facing proxy. Inside P-CSCF there are three sub-threads: one accept
> loop that spawns a per-UE receive thread for each connected UE, and one
> dedicated thread watching the S-CSCF socket to route responses back to
> the correct UE. Messages between nodes are TLV binary over TCP — the
> real SIP text is also generated and written to a PCAP file so Wireshark
> shows the complete call flow."

**"What shared state do you protect and how?"**

> "P-CSCF has two separate mutexes: ue_mtx_ for the UE session registry
> and call_mtx_ for the call routing tables. They are separate because a
> REGISTER and an INVITE can arrive simultaneously from different UEs —
> separate mutexes means these operations never block each other. S-CSCF
> uses shared_mutex for the subscriber registry — reads during INVITE
> routing can happen in parallel while writes during REGISTER are
> exclusive. UE session objects are managed with shared_ptr because the
> session outlives the ue_thread that handles it — the shared_ptr
> guarantees the object is not deleted while any thread still holds a
> reference."

**"What is the subscriber database and how would you scale it?"**

> "In our simulator it is a std::map in RAM — three hardcoded subscribers,
> fast for a demo but lost on restart. In a production IMS, the HSS uses
> Oracle or Cassandra for the permanent subscriber profile, and Redis to
> cache active sessions for sub-millisecond INVITE routing. The key query
> pattern is: write rarely on registration, read on every call setup. That
> is exactly the access pattern shared_mutex optimises for — many
> concurrent readers, rare exclusive writers."

**"How does the 4G EPC connect to the IMS?"**

> "The concrete link is the UE's IP address. The 4G P-GW assigns the UE
> an IP — say 10.0.0.1 — during the EPC Attach. The UE puts that IP in
> the SIP Contact header when it registers with the IMS. So the S-CSCF
> knows to reach UE-A at 10.0.0.1:5060. When a call is established, the
> P-CSCF sends a Diameter Rx AAR to the PCRF, which triggers the P-GW to
> create a dedicated QCI=1 bearer for that voice session. Voice RTP flows
> on QCI=1 — priority guaranteed by the network — while background data
> stays on QCI=9."

---

# Senior System Design Q&A — IMS / MTAS / SIP

---

## Q1. How would you design a high-availability SIP Application Server for 1 million VoLTE subscribers?

### What our simulator does (baseline)

```
One process, one machine, 3 UEs, in-memory subscriber map.
  std::map<string, ImsSubscriber> registry_;   // S-CSCF
  std::map<string, CallState>     calls_;      // active calls
Lost on restart. No redundancy. Fine for a demo.
```

### What production needs — layer by layer

```
Layer 1: SIP load balancer (Session Border Controller)
  Receives all SIP from P-CSCF
  Distributes to MTAS cluster using Call-ID hash
  Ensures same dialog always hits same MTAS node
  Example: Oracle SBC, Ribbon SBC, Cisco CUBE

Layer 2: MTAS cluster (N active nodes)
  Each node handles a slice of subscribers (hash by IMPU)
  Each node is stateful — owns the dialog state for its slice
  Node failure → SBC reroutes to a standby node that replays state

Layer 3: Session state store (shared)
  Redis cluster: stores CallState per Call-ID
  On every state change (INVITE→GRANTED→ACTIVE→BYE):
    MTAS writes to Redis atomically
  If MTAS node dies: new node reads from Redis and continues
  Redis replication: 3-node cluster, 1 primary 2 replicas

Layer 4: Subscriber database (HSS)
  Cassandra cluster: IMPU → subscriber profile, iFC, MSISDN
  Replication factor 3 across data centres
  Cache warm: each MTAS node caches subscriber profiles in local Redis
  Cache miss: query Cassandra (50-100ms), cache for 1 hour

Layer 5: Monitoring
  Prometheus metrics per MTAS node: call rate, error rate, P99 latency
  Alert if P99 > 500ms (SLA breach)
  Auto-scale: Kubernetes HPA adds MTAS pods when CPU > 70%
```

### 1 million subscribers — rough sizing

```
1M subscribers, 5% busy hour = 50,000 concurrent calls
Each call: ~6 SIP messages over 5 min = 0.02 msg/sec/call
50,000 × 0.02 = 1,000 SIP messages/sec total

One MTAS node (8-core, epoll+pool): ~50,000 msg/sec capacity
→ 1,000 / 50,000 = 0.02 = 2% CPU on one node!
→ Run 3 nodes for redundancy (any one can absorb the full load alone)

HSS queries (registration, call setup):
  50,000 call setups/hour = 14 queries/sec
  Cassandra with Redis cache: trivial
```

### What to change in our code

```cpp
// Current: in-memory map, lost on restart
std::map<std::string, CallState> calls_;

// Production Step 1: Redis client for session state
#include <hiredis/hiredis.h>
void saveCallState(const CallState& cs) {
    std::string key = "call:" + cs.call_id;
    std::string val = serialize(cs);           // JSON or protobuf
    redisCommand(redis_, "SET %s %s EX 3600",  // expire after 1 hour
                 key.c_str(), val.c_str());
}
CallState loadCallState(const std::string& call_id) {
    auto* r = (redisReply*)redisCommand(redis_,
                                        "GET call:%s", call_id.c_str());
    return deserialize(r->str);
}
```

### SAY THIS

> "For 1M VoLTE subscribers I would run a clustered MTAS behind a Session
> Border Controller that does Call-ID-based hashing to ensure dialog affinity.
> Each MTAS node owns a slice of subscribers and writes every state transition
> to a Redis cluster so any node can take over on failure. Subscriber profiles
> live in Cassandra with a local Redis cache to avoid a DB hit on every INVITE.
> Three MTAS nodes each sized for the full 1M load gives N+2 redundancy —
> any single node loss is transparent to users."

---

## Q2. Horizontal vs vertical scaling — how does MTAS scale?

### Vertical scaling

```
Add more RAM and CPU to the existing machine.
  Current: 4-core, 8 GB RAM
  Upgraded: 32-core, 128 GB RAM

Pros:  no code change needed, simple
Cons:  hardware limit (you can't buy a machine with 10,000 cores)
       single point of failure remains
       expensive beyond a point

When it helps our sim:
  Right now S-CSCF is single-threaded (one receive loop)
  Vertical scaling gives nothing — the bottleneck is the single thread
  You would need to make S-CSCF multi-threaded first
```

### Horizontal scaling

```
Add more machines (or pods) running the same software.
  Current: 1 MTAS process
  Scaled:  10 MTAS processes on 10 machines

Pros:  linear capacity increase
       no single point of failure
       can scale to any size
Cons:  shared state problem — call state must be externalized (Redis)
       load balancer needed
       more complex operations
```

### How MTAS specifically scales

```
MTAS is STATEFUL → horizontal scaling needs session replication.

Two approaches used in production:

Approach A: Sticky routing (our sim's approach, extended)
  SBC always routes call-id X to MTAS node Y (Call-ID hash)
  Each MTAS node owns its slice exclusively
  No shared state between nodes
  Failure: SBC detects node Y down → reroutes to standby Y'
           standby Y' replays state from Redis

Approach B: Active-active with shared Redis
  Any MTAS node can handle any call
  Every state change written to Redis immediately
  SBC distributes round-robin
  More complex but better load distribution

Ericsson MTAS in production:
  Runs on Blade servers in pairs (active-standby per blade pair)
  Blades share storage (HSS data, session state)
  GeoRed: two sites, active-active across data centres
  Site failure: all traffic routes to surviving site automatically
```

### What to change in our code

```cpp
// Current: S-CSCF is single-threaded, one receive loop
void ScscfNode::receiveLoop() {
    while (!stop_) {
        recvFrame(pcscf_conn_, payload);
        dispatch(payload);   // single thread handles everything
    }
}

// Horizontal-ready: epoll + thread pool + sharded state
// (same design as the 10,000 call architecture)
// Each worker owns calls_[worker_id] — no shared mutex on hot path
// Call-ID hash determines which worker and which Redis key prefix
```

### SAY THIS

> "MTAS is a stateful component — it holds dialog state for every active call.
> Vertical scaling hits a wall quickly and doesn't remove the single point of
> failure. Horizontal scaling is the right answer but requires externalising
> session state to Redis so any node can take over. In practice MTAS scales
> horizontally behind a SBC that does Call-ID hashing for dialog affinity.
> We add nodes as call volume grows — each new node handles a new slice of
> subscribers and writes its state to the shared Redis cluster."

---

## Q3. SIP message throttling and overload control

### Why it matters

```
Without throttling:
  Burst of 100,000 SIP INVITEs arrives (flash crowd, DoS attempt)
  MTAS queue fills up
  Memory exhausted
  Process crashes or all calls fail
  Network outage for all 1M subscribers

With throttling:
  MTAS accepts what it can process
  Rejects the rest cleanly with SIP 503
  Caller retries after Retry-After header tells them when
  System stays stable under overload
```

### Our simulator — no throttling (the gap)

```cpp
// Current acceptLoop — accepts every connection unconditionally
void PcscfNode::acceptLoop() {
    while (!stop_) {
        Socket ue_sock = server_socket_.accept();
        auto ses = std::make_shared<UeSession>();
        ue_threads_.emplace_back([this, ses]{ ueReceiveLoop(ses.get()); });
        // ↑ No limit. 10,000 connections → 10,000 threads → OOM
    }
}

// Current work queue — unbounded
std::queue<Task> queue_;   // grows forever under burst
```

### Token bucket — the standard algorithm

```
Token bucket:
  Bucket holds max N tokens
  Tokens refill at rate R tokens/second
  Each incoming SIP message consumes 1 token
  If bucket empty → reject with 503

Example: N=1000, R=500/sec
  Steady state: 500 INVITEs/sec → all accepted
  Burst: 2000 INVITEs arrive in 1 second
    First 1000: tokens available → accepted
    Next 1000: bucket empty → 503 Retry-After: 2
  After 2 seconds: bucket refilled → accepts again
```

```cpp
// Add to PcscfNode or ScscfNode
class TokenBucket {
    std::atomic<int>  tokens_;
    int               max_tokens_;
    int               refill_rate_;   // per second
    std::thread       refill_thread_;

public:
    TokenBucket(int max, int rate)
        : tokens_(max), max_tokens_(max), refill_rate_(rate) {
        refill_thread_ = std::thread([this]{
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                int add = refill_rate_ / 10;
                int cur = tokens_.load();
                tokens_.store(std::min(cur + add, max_tokens_));
            }
        });
    }

    bool consume() {
        int cur = tokens_.load();
        while (cur > 0) {
            if (tokens_.compare_exchange_weak(cur, cur - 1))
                return true;  // token consumed
        }
        return false;  // bucket empty → reject
    }
};

// In P-CSCF, before forwarding to S-CSCF:
void handleFromUe(UeSession* ses, const std::vector<uint8_t>& payload) {
    if (!throttle_.consume()) {
        // send SIP 503 Service Unavailable back to UE
        sendSip503(ses, "Retry-After: 2");
        Logger::warn("PCSCF", "Throttled INVITE — bucket empty");
        return;
    }
    sendToScscf(payload);
}
```

### Overload levels — graduated response

```
Normal (< 70% CPU):     accept all
Yellow (70-85% CPU):    reject new call setups (INVITE), accept in-call (BYE, ACK)
Red    (85-95% CPU):    reject all new SIP requests
Critical (> 95% CPU):  accept only BYE (let active calls finish, no new ones)

SIP response codes:
  503 Service Unavailable + Retry-After: N seconds  (try again in N sec)
  480 Temporarily Unavailable                        (subscriber busy)
  486 Busy Here                                      (callee busy)
```

### SAY THIS

> "I would use a token bucket per MTAS node — tokens refill at the node's
> sustainable processing rate, each incoming INVITE consumes one token, and
> when the bucket is empty we return 503 with a Retry-After header. SIP
> clients are required by RFC 3261 to honour Retry-After, so they back off
> and retry — the system stays stable. I would also add graduated overload
> levels: at yellow CPU we reject new call setups but still process BYE and
> ACK for active calls, so existing subscribers are not affected. At red we
> stop all new calls. Only at critical do we start dropping in-call traffic,
> and that is the last resort."

---

## Q4. Stateless vs stateful SIP processing — which is MTAS?

### Stateless SIP proxy

```
Does not remember anything between messages.
Each message is processed independently.

Example: a simple SIP proxy / I-CSCF
  Receives INVITE
  Looks up DNS NAPTR to find S-CSCF
  Forwards INVITE
  Forgets it ever saw the INVITE

Pros: can scale infinitely (any instance handles any message)
      no state to replicate or lose
Cons: cannot do anything that requires context across messages
      cannot implement call waiting (needs to know caller is in a call)
      cannot do conference (needs to track all participants)
```

### Stateful SIP — dialog state

```
Remembers the entire dialog: from INVITE until BYE.

Minimum state per dialog:
  Call-ID        "call-AB-001"
  From-tag       "tag-caller"
  To-tag         "tag-callee"    ← set in 200 OK, identifies dialog
  CSeq           current sequence number
  Route set       list of proxies for in-dialog requests
  Dialog state   EARLY / CONFIRMED / TERMINATED

In our code (ScscfNode):
  std::map<std::string, CallState> calls_;   // key = call_id
  struct CallState {
      caller_impu, callee_impu, call_id
      on_hold, in_conference
  };
  Inserted on INVITE, updated on re-INVITE, erased on BYE.
```

### MTAS is stateful — and more than dialog state

```
MTAS (Multimedia Telephony Application Server) maintains:

1. Dialog state (above) — standard SIP UA state
2. Subscription state — which UEs are registered, their service profile
3. Service state — per subscriber:
     call_waiting_active: bool
     call_forwarding_number: string
     barring_flags: OIB, BAIC, BIC-Roam
     conference_participants: vector<string>
     hold_dialogs: vector<CallID>

In our code (MtasState namespace + calls_ map in S-CSCF):

  namespace MtasState {
      std::set<std::string> barred;   // OIB barring state per IMPU
  }

  // inside ScscfNode::handleInvite():
  if (MtasState::isBarred(caller_impu)) {
      sendSip603(caller, "OIB barring active");
      return;
  }
  // MTAS checked service state before allowing the call

Why this matters for scaling:
  Stateful = you cannot just add a new node and route any call to it
  The new node has no state for in-flight calls
  → Need Redis or session replication (see Q1 and Q2)
```

### Stateless components in our IMS (for comparison)

```
I-CSCF (not implemented in our sim but conceptually):
  Stateless SIP proxy
  Receives REGISTER, does UAR/UAA to HSS, picks S-CSCF, forwards
  Receives INVITE for incoming calls, does LIR/LIA to HSS, forwards
  Forgets everything after forwarding
  Can be load balanced round-robin with no session affinity

P-CSCF:
  Mostly stateless for SIP routing
  Does maintain UE socket mapping and call routing tables
  But those are transport-level, not SIP dialog state
```

### SAY THIS

> "A stateless SIP proxy processes each message in isolation — it can scale
> horizontally with no session affinity because it has no memory of previous
> messages. MTAS is stateful: it maintains dialog state from INVITE to BYE,
> subscription state per registered UE, and service state — call waiting flags,
> barring rules, conference participants. In our simulator this is the calls_
> map in S-CSCF and the MtasState namespace for barring. Because MTAS is
> stateful, horizontal scaling requires session replication to Redis so any
> node can pick up a dialog mid-flight. You cannot just round-robin SIP
> messages to a stateful AS — you need Call-ID hashing for dialog affinity."

---

## Q5. Call processing pipeline for an IMS AS — thread model, queue, stages

### Our current pipeline (single-threaded, works for demo)

```
P-CSCF TCP socket
    │
    ▼
ScscfNode::receiveLoop()          ← ONE thread does everything
    │
    ├── recvFrame()               STAGE 1: receive raw bytes
    ├── MessageReader::msgType()  STAGE 2: decode TLV header
    ├── switch(type)              STAGE 3: dispatch
    │     ├── handleRegister()   STAGE 4A: process registration
    │     ├── handleInvite()     STAGE 4B: process call setup
    │     │     ├── invokeMtas() STAGE 4B-i:  MTAS service logic
    │     │     ├── sendCxSAR()  STAGE 4B-ii: HSS query (BLOCKS)
    │     │     └── routeToCallee()
    │     └── handleBye()        STAGE 4C: call teardown
    └── (back to recvFrame)

Problem: HSS query at stage 4B-ii blocks the ENTIRE pipeline.
While waiting for HSS response (5-50ms), no other messages processed.
At 100 calls/sec: 100 × 50ms = 5000ms of blocked time per second.
```

### Production pipeline — staged, async, sharded

```
Stage 1: RECEIVE (1 epoll thread)
  epoll_wait() on all sockets
  Reads raw bytes into buffer
  Enqueues to Stage 2 queue
  Never blocks, never processes

Stage 2: DECODE (N decode workers, stateless)
  Read TLV frame from Stage 1 queue
  Parse message type and all IEs
  Extract Call-ID for routing
  Push (call_id, parsed_msg) to Stage 3

Stage 3: ROUTE / DISPATCH (1 router thread)
  hash(call_id) % num_workers → picks worker N
  Push to worker N's dedicated queue
  Ensures all messages for one dialog go to same worker

Stage 4: PROCESS (32 stateful workers, each owns a shard)
  Worker N owns: calls_[N], barring_state_[N]
  Processes messages for its shard — NO LOCK NEEDED
  Service logic: invoke MTAS checks (synchronous, in-memory)
  If HSS needed: fire async request to Stage 5, suspend this task

Stage 5: HSS/DB POOL (8 threads, blocking allowed)
  Execute Cassandra / Redis query
  On result: re-enqueue task to Stage 4 worker that suspended it

Stage 6: SEND (1 sender thread or per-worker)
  Write response to P-CSCF socket
```

```
In code (building on our epoll design):

struct Task {
    std::string         call_id;
    SipMsgType          type;
    ParsedSipMsg        msg;
    std::promise<void>* continuation;  // for async HSS resume
};

// Stage 3: router
void routerThread(WorkQueue& in, PerWorkerQueue workers[32]) {
    while (true) {
        Task t = in.pop();
        int w = std::hash<std::string>{}(t.call_id) % 32;
        workers[w].push(std::move(t));
    }
}

// Stage 4: worker (owns shard w)
void workerThread(int w, PerWorkerQueue& myQ, HssPool& hss) {
    while (true) {
        Task t = myQ.pop();
        switch (t.type) {
            case SIP_INVITE:
                if (needsHssQuery(t)) {
                    // ASYNC: don't block this worker
                    hss.queryAsync(t.msg.impu, [&myQ, t](Profile p) mutable {
                        t.profile = p;
                        myQ.pushFront(t);   // re-enqueue with data
                    });
                    break;  // worker immediately takes next task
                }
                processInvite(w, t);
                break;
            case SIP_BYE:
                processBye(w, t);
                break;
        }
    }
}
```

### What to add to our simulator to show this

```cpp
// In ScscfNode: replace single receive loop with staged dispatch
// Minimum change: add a thread pool for INVITE processing
// so HSS blocking doesn't stall REGISTER processing

class ScscfNode {
    ThreadPool   invite_pool_{8};   // parallel INVITE processing
    ThreadPool   reg_pool_{4};      // parallel REGISTER + HSS

    void receiveLoop() {
        while (!stop_) {
            recvFrame(pcscf_conn_, payload);
            auto type = decodeType(payload);
            if (type == SIP_INVITE) {
                invite_pool_.submit([this, payload]{ handleInvite(payload); });
            } else if (type == SIP_REGISTER) {
                reg_pool_.submit([this, payload]{ handleRegister(payload); });
            } else {
                dispatch(type, payload);   // ACK/BYE are fast, inline is fine
            }
        }
    }
};
// Now: 8 INVITEs process in parallel, HSS blocking in one does not stall others
```

### SAY THIS

> "Our simulator uses a single receive loop — simple but the HSS query blocks
> everything. For production I would use a staged pipeline: one epoll thread
> receives all traffic, a router thread shards tasks by Call-ID hash to 32
> stateful workers, and a separate HSS pool handles blocking DB queries
> asynchronously so workers are never blocked. Workers own their shard of
> call state — no mutex on the hot path. This gives linear throughput scaling
> by adding workers and keeps P99 latency predictable even under burst load."

---

## Q6. Active-active vs active-standby HA — which is preferred for MTAS?

### Active-standby

```
Setup:
  Node A: PRIMARY — handles all traffic
  Node B: STANDBY — receives replicated state, handles nothing

Failover:
  Node A dies (hardware failure, OOM, crash)
  Load balancer detects failure (health check timeout ~3-10s)
  Node B promoted to PRIMARY — starts handling traffic
  Calls in-flight during failover: some drop (depends on replication lag)

State replication:
  Node A writes every state change to shared storage (Redis/NFS/DB)
  Node B reads state from shared storage on promotion
  Replication lag: 10-100ms typically

Pros:
  Simple — Node B never needs to process during normal operation
  No shared-state race conditions (only one writer at a time)
  Easy to reason about consistency

Cons:
  Node B is wasted capacity during normal operation
  Failover takes 3-10 seconds (health check interval)
  Calls in-flight during failover may drop

Used by: Ericsson MTAS on blade servers (active-standby blade pairs)
```

### Active-active

```
Setup:
  Node A: handles 50% of subscribers (hash range 0-499,999)
  Node B: handles 50% of subscribers (hash range 500,000-999,999)
  Both handle traffic simultaneously

Failover:
  Node A dies
  Load balancer detects failure
  Node B expands to handle ALL subscribers (0-999,999)
  Node B reads Node A's state from Redis to serve Node A's calls

Pros:
  Both nodes used — no wasted capacity
  Each node only needs to scale to handle the other's load
  (each sized for 100% load, normally at 50%)
  Failover is faster — Node B just expands its hash range

Cons:
  More complex — state must be shared between nodes in real time
  Both nodes write to Redis — potential write conflicts
  More expensive (two full-capacity nodes instead of one active + one standby)

Used by: Nokia IMS (geo-redundant active-active across data centres)
```

### Which is preferred for MTAS?

```
The industry answer: IT DEPENDS on the deployment.

Single data centre:
  Active-standby is simpler and sufficient
  MTAS is already complex — avoid additional shared-state complexity
  Failover in 3-10s is acceptable for most operators

Geo-redundant (two data centres):
  Active-active is required — you WANT both sites handling traffic
  A standby site doing nothing defeats the purpose of geo-redundancy
  State replication via stretch Redis cluster or database replication
  Ericsson IMS GeoRed uses active-active between sites

For a candidate answer: say ACTIVE-STANDBY within a site,
ACTIVE-ACTIVE across sites (geo-redundancy).
```

### What this means for our code

```cpp
// Our simulator: no HA at all
// To add active-standby:

// 1. All state writes go to Redis (not just in-memory map)
void ScscfNode::handleInvite(const vector<uint8_t>& payload) {
    // ... process ...
    CallState cs{caller, callee, call_id};
    calls_[call_id] = cs;                      // local
    redis_.set("call:" + call_id, serialize(cs)); // replicated ✓
}

// 2. On startup (if I am the new primary after failover):
void ScscfNode::restoreFromRedis() {
    auto keys = redis_.keys("call:*");
    for (auto& k : keys) {
        CallState cs = deserialize(redis_.get(k));
        calls_[cs.call_id] = cs;
    }
    Logger::sys("Restored " + to_string(calls_.size()) + " active calls from Redis");
}

// 3. Health check endpoint (load balancer pings this):
// HTTP GET /health → 200 OK if serving, 503 if overloaded/starting
```

### SAY THIS

> "For a single data centre I prefer active-standby for MTAS — it is simpler
> to operate, Node B only needs to replay state from Redis on failover, and
> there is only one writer so no consistency conflicts. Active-standby gives
> ~5 second failover which is acceptable for most SLAs. For geo-redundancy
> across two data centres I would use active-active, where each site handles
> its region's traffic and the other site takes over on site failure. The key
> enabler for both is externalising all session state to Redis — without that
> neither HA model works because state lives only in the crashed process's
> memory."

---

## Combined Interview Cheat Sheet

| Question | One-line answer | Reference in our code |
|---|---|---|
| Design for 1M subscribers | Clustered MTAS + Redis session store + Cassandra HSS | `calls_` map → Redis, `registry_` → Cassandra |
| Horizontal vs vertical | Horizontal preferred; needs session replication | `shared_mutex registry_mtx_` → Redis replication |
| Throttling | Token bucket per node; SIP 503 + Retry-After | Add `TokenBucket throttle_` before `sendToScscf()` |
| Stateful vs stateless | MTAS is stateful (dialog + service state) | `calls_` map, `MtasState::barred` set |
| Call processing pipeline | epoll + staged workers sharded by Call-ID | `receiveLoop()` → add `ThreadPool invite_pool_` |
| Active-active vs standby | Standby within site, active-active geo-redundant | All state writes → Redis `restoreFromRedis()` on boot |
