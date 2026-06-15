# Level 3 — Deep Dive: C++17, Threading, TLV Encoding, Design Patterns

> For 5-10yr engineers. Code-level walkthrough of what makes this interesting.

---

## Threading Model

```
main thread ── CLI + metrics + thread pool controller
    │
    ├── hss_th    ── HssNode::run()  → receiveLoop()
    ├── pcrf_th   ── PcrfNode::run() → receiveLoop()
    ├── sgw_th    ── SgwNode::run()  → receiveLoop()
    ├── pgw_th    ── PgwNode::run()  → receiveLoop()
    ├── enb_th    ── EnbNode::run()  → receiveLoop() + commandLoop()
    └── mme_th    ── MmeNode::run()
            ├── enbReceiveLoop()   [mme_th]   main MME thread
            └── hssReceiveLoop()   [hss_rx_th] spawned by MME
```

### The Async Handoff Problem (condition_variable)

MME's auth flow is split across two threads:
- `mme_th` sends AIR to HSS and needs to block until AIA arrives
- `hss_rx_th` receives AIA and needs to unblock `mme_th`

```cpp
// hss_rx_th (PRODUCER) — in MmeNode::hssReceiveLoop()
{
    std::lock_guard<std::mutex> lk(pending_auth_mutex_);
    pending_auth_[imsi] = av;   // store auth vectors
}
pending_auth_cv_.notify_one(); // wake up mme_th

// mme_th (CONSUMER) — in MmeNode::handleInitialUEMsg()
std::unique_lock<std::mutex> lk(pending_auth_mutex_);
pending_auth_cv_.wait(lk, [&]{
    return pending_auth_.count(imsi) > 0 || stop_.load();
});
// condition_variable: releases lock during wait, reacquires on wake
// Predicate prevents spurious wakeups
```

**Interview Q:** "Why predicate in cv.wait?"
Spurious wakeups are real — POSIX allows threads to wake without notify.
The predicate ensures we only proceed when the actual condition is true.

---

## Sharded UE Context Store

Problem: Global mutex on UE store = serialized lookups = latency bottleneck.
Solution: 64 buckets, each with its own `shared_mutex`.

```cpp
// ue_context_store.h
class UeContextStore {
    static constexpr int SHARDS = 64;
    struct Shard {
        std::shared_mutex               mtx;
        std::unordered_map<uint32_t, std::shared_ptr<UeContext>> map;
    };
    std::array<Shard, SHARDS> shards_;

    Shard& shard(uint32_t id) { return shards_[id % SHARDS]; }

public:
    // Multiple readers: shared_lock (concurrent reads allowed)
    std::shared_ptr<UeContext> find(uint32_t id) {
        auto& s = shard(id);
        std::shared_lock<std::shared_mutex> lk(s.mtx);
        auto it = s.map.find(id);
        return it != s.map.end() ? it->second : nullptr;
    }

    // Single writer: unique_lock (exclusive)
    uint32_t insert(std::shared_ptr<UeContext> ctx) {
        uint32_t id = next_id_.fetch_add(1, std::memory_order_relaxed);
        auto& s = shard(id);
        std::unique_lock<std::shared_mutex> lk(s.mtx);
        s.map[id] = std::move(ctx);
        return id;
    }
};
```

**Interview Q:** "Why 64 shards?"
Reduces contention by 64x for read-heavy workloads. Typical core count
(8-16) means most concurrent lookups hit different shards entirely.
Trade-off: higher memory (64 mutexes) vs lower contention.

---

## Lock-Free ID Allocation

```cpp
// P-GW: atomic TEID and IP allocation — zero mutex overhead
std::atomic<uint32_t> next_teid_{1};
std::atomic<uint32_t> next_ip_suffix_{2};  // 10.0.0.2, 10.0.0.3...

uint32_t alloc_teid() {
    return next_teid_.fetch_add(1, std::memory_order_relaxed);
}
// memory_order_relaxed: no ordering guarantees needed —
// each TEID is independent (no dependency between allocations)
```

---

## Binary TLV Wire Format

Every message is encoded as:
```
[2B MsgType] [4B SeqNum] [TLV...TLV]
```

Each TLV:
```
[2B Tag] [2B Length] [Length bytes of Value]
```

```cpp
// tlv.h — MessageWriter
class MessageWriter {
    std::vector<uint8_t> buf_;
public:
    MessageWriter(MessageType type, uint32_t seq) {
        // Header: type (2B) + seq (4B) = 6 bytes
        buf_.push_back(uint8_t(uint16_t(type) >> 8));
        buf_.push_back(uint8_t(uint16_t(type)));
        buf_.push_back(uint8_t(seq >> 24)); // ... 4 bytes seq
    }
    void writeU64(Tag tag, uint64_t val) {
        pushU16(uint16_t(tag));
        pushU16(8);             // length = 8 bytes
        for (int i=56; i>=0; i-=8)
            buf_.push_back(uint8_t(val >> i));
    }
    std::vector<uint8_t> frame() { return buf_; }
};
```

**vs. real 3GPP encoding:**
- Real S1AP = ASN.1 PER (Packed Encoding Rules) — bit-level, complex
- Real Diameter = AVP format with vendor IDs — 8-byte overhead per field
- Real GTPv2 = specific IE types per field — tight binary format
- Our TLV = simpler but isomorphic — same information, easier to parse

For Wireshark, our `pcap_writer.cpp` wraps outgoing messages in real
Diameter/GTPv2 headers so the protocol column shows correctly.

---

## Design Patterns Used

### 1. RAII — Socket Wrapper
```cpp
class Socket {
    int fd_ = -1;
public:
    ~Socket() { if (fd_ >= 0) ::close(fd_); } // guaranteed cleanup
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_ = -1; }
    Socket& operator=(Socket&&) noexcept;
    Socket(const Socket&) = delete; // no copies — unique ownership
};
```
No `close()` forgotten in error paths. Exception-safe.

### 2. Flyweight — Subscriber Profile
```cpp
// ProfileRegistry: immutable profiles shared across UEs
class ProfileRegistry {
    std::unordered_map<std::string, std::shared_ptr<SubscriberProfile>> cache_;
public:
    std::shared_ptr<SubscriberProfile> get(const std::string& apn) {
        auto it = cache_.find(apn);
        if (it != cache_.end()) return it->second; // shared — not copied
        auto p = std::make_shared<SubscriberProfile>(apn);
        cache_[apn] = p;
        return p;
    }
};
// 1000 UEs on same APN → 1 SubscriberProfile object, 1000 shared_ptrs
// saves: 1000 × sizeof(SubscriberProfile) of memory
```

### 3. Producer-Consumer — CLI → eNB Queue
```cpp
// EnbNode: command queue
std::queue<std::string>  cmd_queue_;
std::mutex               cmd_mtx_;
std::condition_variable  cmd_cv_;

// CLI thread (producer)
void submitCommand(const std::string& cmd) {
    std::lock_guard<std::mutex> lk(cmd_mtx_);
    cmd_queue_.push(cmd);
    cmd_cv_.notify_one();
}

// eNB thread (consumer)
void commandLoop() {
    std::unique_lock<std::mutex> lk(cmd_mtx_);
    cmd_cv_.wait(lk, [&]{ return !cmd_queue_.empty() || stop_; });
    auto cmd = cmd_queue_.front(); cmd_queue_.pop();
    // process...
}
```

### 4. Thread Pool — BULK command
```cpp
// thread_pool.h
class ThreadPool {
    std::vector<std::thread>          workers_;
    std::queue<std::function<void()>> tasks_;
    std::mutex                        mtx_;
    std::condition_variable           cv_;
    std::atomic<bool>                 stop_{false};
public:
    ThreadPool(int n, const char* name) {
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this]{ workerLoop(); });
    }
    void submit(std::function<void()> f) {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push(std::move(f));
        cv_.notify_one();
    }
private:
    void workerLoop() {
        while (true) {
            std::unique_lock<std::mutex> lk(mtx_);
            cv_.wait(lk, [&]{ return !tasks_.empty() || stop_; });
            if (stop_ && tasks_.empty()) return;
            auto task = std::move(tasks_.front()); tasks_.pop();
            lk.unlock();
            task();  // execute outside lock — max concurrency
        }
    }
};
```

### 5. State Machine — EMM States
```cpp
enum class EmmState {
    DEREGISTERED,       // before attach
    REGISTERED_INITIATED, // attach request received
    AUTH_PENDING,       // AIR sent, waiting AIA
    SESSION_PENDING,    // GTP session being created
    REGISTERED          // fully attached
};
```

Each state transition is logged. Invalid transitions are rejected.

---

## Smart Pointers — Where and Why

| Usage | Type | Why |
|-------|------|-----|
| UeContext | `shared_ptr` | Shared between mme_th and hss_rx_th |
| SubscriberProfile | `shared_ptr` | Flyweight — multiple UEs point to same object |
| Metrics | `shared_ptr` | Shared between main and mme_th |
| Socket | `unique_ptr` equivalent | Move-only, single owner |

**Never use raw `new` in this codebase.** All heap objects via smart pointers.

---

## PCAP Writer — How Wireshark Shows Real Protocols

```cpp
// For Diameter (port 3868) — RFC 6733 §3
// Wireshark sees port 3868 + Diameter header → shows "Diameter"

void PcapWriter::writeDiameter(DiameterCmd cmd, ...) {
    vector<uint8_t> dia;
    dia.push_back(1);                   // Version = 1
    putU24(dia, total_len);             // Message Length (3 bytes!)
    dia.push_back(is_req ? 0x80 : 0);  // Flags: R=request bit
    putU24(dia, uint32_t(cmd));         // Command Code (318=AIR/AIA)
    putU32(dia, uint32_t(app));         // Application-ID (16777251=S6a)
    putU32(dia, hop_by_hop_id);
    putU32(dia, end_to_end_id);
    // Wrap in IP + TCP frame, write to .pcap file
}
```

**For GTPv2 (port 2123) — TS 29.274**
```cpp
gtp[0] = 0x48;           // version=2, T=1 (TEID present)
gtp[1] = msg_type;       // 32=Create Session Req, 33=Rsp
putU16(gtp, length);     // total - 4
putU32(gtp, teid);
putU24(gtp, seq_no);
gtp.push_back(0);        // spare
```

Open `mme_capture.pcap` in Wireshark after running `CR 1`.
Filter: `diameter || gtpv2 || sip`

---

## Windows Portability (CMakeLists.txt)

```cmake
if(WIN32)
    add_compile_definitions(_WIN32_WINNT=0x0601)
    target_link_libraries(mme_sim ws2_32)  # Winsock2
endif()
```

In socket_wrapper.h:
```cpp
#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
#endif
```
