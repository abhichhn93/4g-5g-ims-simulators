# IMS/VoLTE Performance Scenarios — Interview Prep Guide

> 5 classic performance problems in multithreaded telecom systems, demonstrated on the IMS/SIP simulator. Each scenario shows a **buggy** version, a **fixed** version, and gives you the exact words to say in an interview.

---

## Quick Reference

| # | Problem | IMS Context | Bug | Fix | Diagnose With |
|---|---------|-------------|-----|-----|---------------|
| 1 | CPU 100% | P-CSCF queue polling | Busy-wait loop | `condition_variable::wait()` | `top -H -p PID`, `perf top` |
| 2 | Lock Contention | Session table mutex | One global lock | Shard by `hash(Call-ID)` | `pidstat -t`, `perf lock` |
| 3 | Memory Leak | SIP dialog contexts | Raw `new`/`delete` | `unique_ptr` + bounded queue | `pidstat -r`, `valgrind` |
| 4 | Thread Starvation | P-CSCF event loop | Blocking I/O, 2 workers | Pool sized to cores, async | Thread states, p99 latency |
| 5 | Slow Data Path | Session lookup + parsing | O(n) vector scan + copies | `unordered_map` + `string_view` | `perf record -g` + flamegraph |

---

## How to Run

```bash
# Build
cd 4g-5g-ims-simulators/ims-simulator
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# Run all 5 scenarios (buggy → fixed, with metrics)
./perf_scenarios

# Run one scenario
./perf_scenarios --scenario 1

# List all scenarios
./perf_scenarios --list
```

---

## Scenario 1: CPU 100% — Busy-Wait Polling

### IMS Context
The P-CSCF processes SIP messages from a queue. In the buggy version, the worker thread **polls continuously** even when the queue is empty — burning CPU cycles.

### Symptom
- CPU at 100% even with no traffic
- Fan spinning, server hot, cloud CPU bill high
- `top` shows one core pinned

### How to Run
```bash
./perf_scenarios --scenario 1
```

### Diagnosis Commands
```bash
top -H -p <PID>           # See which thread burns CPU
perf top                  # See which function dominates
perf record -g -p <PID>; perf flamegraph  # Get flamegraph
```

### Before (Buggy) — ~10 lines
```cpp
// BUG: Busy-wait — polls queue with no backoff, burns CPU
void workerLoop() {
    while (running_) {
        std::lock_guard<std::mutex> lk(queue_mtx_);
        if (!queue_.empty()) {
            process(queue_.front());
            queue_.pop();
        }
        // Falls through immediately → tight loop → 100% CPU
        std::this_thread::yield();  // Doesn't help much
    }
}
```

### After (Fixed) — ~10 lines
```cpp
// FIX: Block on condition_variable — thread sleeps until notified
void workerLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lk(queue_mtx_);
        queue_cv_.wait_for(lk, std::chrono::milliseconds(100),
            [this] { return !queue_.empty() || !running_; });
        if (!queue_.empty()) {
            process(queue_.front());
            queue_.pop();
        }
    }
}
```

### Design Principle
**Event-driven wait** — never poll when you can be notified.

### SAY THIS (Interview Script)
> "In our P-CSCF, I found a worker thread burning 100% CPU on an idle queue. `top -H` showed the thread pinned, and `perf top` confirmed it was stuck in the queue-polling loop. The fix was replacing the busy-wait with a `condition_variable::wait()` — the thread now sleeps when idle and gets woken by `notify_one()` when a SIP message arrives. CPU dropped from 100% to ~5% on idle, and we saw no latency impact because the CV wakeup is sub-microsecond."

---

## Scenario 2: Lock Contention — Global Mutex

### IMS Context
The S-CSCF maintains a session table mapping `Call-ID → CallSession`. In the buggy version, **one global mutex** protects the entire table — every worker thread contends on the same lock.

### Symptom
- Throughput plateaus as you add threads
- High context-switch rate
- `pidstat -t` shows many voluntary/involuntary switches

### How to Run
```bash
./perf_scenarios --scenario 2
```

### Diagnosis Commands
```bash
pidstat -t -p <PID> 1     # Per-thread context switches
perf lock record          # Lock contention profiling
perf lock report          # Show most-contended locks
```

### Before (Buggy) — ~12 lines
```cpp
// BUG: One global mutex — all N worker threads serialize here
std::mutex global_mtx;
std::unordered_map<std::string, CallSession> sessions;

void processCall(const std::string& call_id) {
    global_mtx.lock();           // ALL threads wait here
    contentions_++;              // Count contentions
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    sessions[call_id] = session; // Critical section is slow
    global_mtx.unlock();
}
```

### After (Fixed) — ~12 lines
```cpp
// FIX: Shard by hash(Call-ID) — 16 independent mutexes
static constexpr int NUM_SHARDS = 16;
struct Shard { std::mutex mtx; std::unordered_map<std::string, CallSession> sessions; };
std::vector<std::unique_ptr<Shard>> shards_(NUM_SHARDS);

void processCall(const std::string& call_id) {
    size_t idx = std::hash<std::string>{}(call_id) % NUM_SHARDS;
    shards_[idx]->mtx.lock();    // Only threads hitting SAME shard contend
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    shards_[idx]->sessions[call_id] = session;
    shards_[idx]->mtx.unlock();
}
```

### Design Principle
**Shard state** — partition data so threads work on independent subsets.

### SAY THIS
> "Our S-CSCF session table used a single global mutex, and with 8 worker threads, lock contention killed throughput. `pidstat -t` showed thousands of context switches per second, and `perf lock` confirmed the session table mutex was the hotspot. I sharded the table into 16 independent mutexes keyed by `hash(Call-ID) % 16`. This reduced lock contention by ~90% and nearly linearized throughput scaling from 2x to 7x when going from 2 to 8 threads."

---

## Scenario 3: Memory Leak — Raw Pointers on Error Paths

### IMS Context
SIP dialog contexts are allocated when a call starts. In the buggy version, raw `new`/`delete` is used, and on error paths (e.g., call rejection, timeout), the `body` buffer is **not freed**.

### Symptom
- RSS grows monotonically over time
- OOM after hours/days of operation
- More leaks under high error rate (rejected calls, timeouts)

### How to Run
```bash
./perf_scenarios --scenario 3
```

### Diagnosis Commands
```bash
pidstat -r -p <PID> 1       # RSS growth over time
valgrind --leak-check=full  # In test environment
ASan + LSan                 # AddressSanitizer + LeakSanitizer
```

### Before (Buggy) — ~15 lines
```cpp
// BUG: Raw pointer — body leaks on error paths
struct DialogContext {
    std::string call_id;
    char* body;  // raw pointer — LEAKED on error
};

void createDialog(const std::string& call_id) {
    DialogContext* ctx = new DialogContext();
    ctx->body = new char[1024];  // Allocated
    dialogs_.push_back(ctx);
}

void simulateError(const std::string& call_id) {
    // BUG: delete ctx but FORGET delete[] body
    delete ctx;  // body buffer leaks!
}
```

### After (Fixed) — ~15 lines
```cpp
// FIX: unique_ptr — auto-cleanup on ALL exit paths
struct DialogContext {
    std::string call_id;
    std::unique_ptr<char[]> body;  // RAII — auto-deleted
};

void createDialog(const std::string& call_id) {
    auto ctx = std::make_unique<DialogContext>();
    ctx->body = std::make_unique<char[]>(1024);
    dialogs_.push_back(std::move(ctx));
}

void simulateError(const std::string& call_id) {
    // FIX: unique_ptr destructor auto-deletes body
    dialogs_.erase(it);  // No manual cleanup needed
}
```

### Design Principle
**RAII + smart pointers** — resource ownership encoded in types, cleanup guaranteed on all paths.

### SAY THIS
> "We had a memory leak in SIP dialog handling — on error paths (call rejection, timeout), we'd delete the DialogContext struct but forget to `delete[]` the body buffer. Under load with a 30% error rate, RSS grew by ~500MB/hour. I replaced raw pointers with `unique_ptr<char[]>` for the body, so the destructor handles cleanup automatically on any exit path — normal, error, or exception. Valgrind showed zero leaks after the fix."

---

## Scenario 4: Thread Pool Starvation — Blocking on Hot Path

### IMS Context
The P-CSCF event loop handles SIP messages. In the buggy version, the **same 2 worker threads** that process messages also do blocking Diameter Rx calls to the PCRF — blocking the hot path.

### Symptom
- Latency spikes under load (p99 goes from 1ms to 500ms+)
- Tasks queue up, timeouts increase
- Throughput drops as blocking calls increase

### How to Run
```bash
./perf_scenarios --scenario 4
```

### Diagnosis Commands
```bash
# Check thread states
cat /proc/<PID>/task/*/stat | awk '{print $3}'  # R=running, S=sleeping, D=disk-sleep

# Latency percentiles
hist -p <PID>  # or use custom latency histogram

# Watch queue depth grow
watch -n1 'ps -T -p <PID> | wc -l'
```

### Before (Buggy) — ~10 lines
```cpp
// BUG: 2 workers + blocking I/O on the hot path
int num_workers = 2;  // Too few!
std::vector<std::thread> workers;

for (int i = 0; i < num_workers; i++) {
    workers.emplace_back([this] {
        while (running_) {
            auto task = queue_.pop();
            task();  // If task blocks for 10ms, both workers are stuck
        }
    });
}
```

### After (Fixed) — ~10 lines
```cpp
// FIX: Pool sized to cores + offload blocking work
int num_workers = std::thread::hardware_concurrency();  // Scale with CPU
std::vector<std::thread> blocking_workers;  // Separate pool for blocking I/O

// Hot path: non-blocking work on main pool
// Blocking work (Diameter Rx, HSS queries): separate pool
blocking_workers.emplace_back([this] {
    while (running_) {
        auto blocking_task = blocking_queue_.pop();
        blocking_task();  // Doesn't starve the hot path
    }
});
```

### Design Principle
**Keep the hot path non-blocking** — isolate blocking I/O to separate threads.

### SAY THIS
> "Our P-CSCF had only 2 worker threads handling both SIP message processing and blocking Diameter Rx calls to PCRF. When a Diameter call blocked for 10ms, both workers were stuck, and SIP messages queued up — p99 latency spiked to 500ms. I split the pools: the hot path got `hardware_concurrency()` threads for non-blocking SIP processing, and a separate small pool handled blocking Diameter/HSS I/O. p99 latency dropped from 500ms to 2ms under the same load."

---

## Scenario 5: Inefficient Data Path — O(n) Lookup + Copies

### IMS Context
The S-CSCF looks up call sessions by `Call-ID` for every SIP message. In the buggy version, sessions are stored in a `std::vector` — **O(n) linear scan** per lookup. Additionally, the SIP message body is **copied** even when only a small field is needed.

### Symptom
- CPU usage scales with session count (not constant)
- `perf` shows time in `memcmp`/`strcmp` during session lookup
- High allocation rate in malloc profiler

### How to Run
```bash
./perf_scenarios --scenario 5
```

### Diagnosis Commands
```bash
perf record -g -p <PID> sleep 10   # Record CPU profile
perf report --stdio                 # See hot functions
# Generate flamegraph:
perf script | stackcollapse-perf.pl | flamegraph.pl > flame.svg
```

### Before (Buggy) — ~15 lines
```cpp
// BUG: O(n) lookup in vector + unnecessary string copy
std::vector<CallSession> sessions;  // O(n) scan!

CallSession* findSession(const std::string& call_id) {
    for (auto& s : sessions) {           // O(n) — scans ALL sessions
        if (s.call_id == call_id) return &s;
    }
    return nullptr;
}

void processMessage(const SipMessage& msg) {
    std::string body_copy = msg.body;  // Full copy — unnecessary allocation
    CallSession* s = findSession(msg.call_id);  // O(n) lookup
}
```

### After (Fixed) — ~15 lines
```cpp
// FIX: O(1) hash lookup + zero-copy (string_view)
std::unordered_map<std::string, CallSession> sessions;  // O(1) lookup

CallSession* findSession(const std::string& call_id) {
    auto it = sessions.find(call_id);  // O(1) hash lookup
    return it != sessions.end() ? &it->second : nullptr;
}

void processMessage(const SipMessage& msg) {
    std::string_view body_view = msg.body;  // Zero-copy reference
    CallSession* s = findSession(msg.call_id);  // O(1) lookup
}
```

### Design Principle
**Zero-copy + indexed lookup** — avoid copies with `string_view`, use hash maps for O(1) access.

### SAY THIS
> "Our S-CSCF session lookup was O(n) — we stored sessions in a vector and scanned linearly for each SIP message. With 10K active calls, `perf` showed 40% of CPU in `strcmp` inside the lookup loop. I replaced the vector with `unordered_map<string, CallSession>` for O(1) hash lookup, and used `string_view` to avoid copying the SIP body when we only needed to read a field. CPU per message dropped by 60% and latency became constant regardless of session count."

---

## Appendix: Quick Diagnosis Cheat Sheet

| Symptom | First Command | What to Look For |
|---------|--------------|------------------|
| High CPU | `top -H -p PID` | Thread with high `%CPU` |
| High CPU | `perf top` | Function at top of profile |
| Lock contention | `pidstat -t -p PID 1` | High `cswch/s` (voluntary switches) |
| Lock contention | `perf lock record` | Most-contended lock |
| Memory growth | `pidstat -r -p PID 1` | RSS column increasing |
| Memory leak | `valgrind --leak-check=full` | "definitely lost" bytes |
| Latency spikes | Custom histogram or `pidstat` | High `delayacct` wait time |
| Slow data path | `perf record -g` + flamegraph | Hot functions in lookup/copy |

---

## Files Added

| File | Purpose |
|------|---------|
| `src/perf_scenarios.h` | Header with all 5 scenario classes |
| `src/perf_scenarios.cpp` | Implementation (buggy + fixed for each) |
| `src/perf_scenarios_main.cpp` | CLI entry point |
| `CMakeLists.txt` | Added `perf_scenarios` target |
| `docs/INTERVIEW_PERF_SCENARIOS.md` | This document |

### One-line CMakeLists.txt change:
```cmake
add_executable(perf_scenarios src/perf_scenarios_main.cpp src/perf_scenarios.cpp)
target_include_directories(perf_scenarios PRIVATE src)
target_link_libraries(perf_scenarios Threads::Threads ${PLATFORM_LIBS})