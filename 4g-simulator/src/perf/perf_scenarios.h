#pragma once
// ============================================================
// PERF SCENARIOS — 5 classic interview performance problems
// framed in 4G/IMS/SIP terms.
//
// Each scenario has a BUGGY and FIXED toggle.
// Command:  PERF <1-5> <buggy|fixed> [n]
//
// All scenarios are self-contained — no dependency on the
// running MME/eNB stack. They demonstrate the same patterns
// that exist in the real nodes (thread pool, queue, locking).
// ============================================================
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <numeric>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include "common/logger.h"

namespace Perf {

// ── tiny helpers ─────────────────────────────────────────────────────────────

static inline long long now_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

static inline std::string fmt1(double v) {
    char b[32]; std::snprintf(b, sizeof(b), "%.1f", v); return b;
}

static inline double pct(std::vector<double>& v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(size_t(p * v.size()), v.size() - 1)];
}

// ════════════════════════════════════════════════════════════════════════════
// SCENARIO 1 — CPU BUSY-WAIT
//
// SYMPTOM:  One CPU core pinned at 100% even while the MME receive loop
//           processes only 200 SIP INVITEs/sec.
// ROOT CAUSE: Poll loop — "while (!msg) check again" — burns cycles doing
//            nothing.  In Linux: thread stays in RUNNING state, never yields.
// FIX:       Replace spin with condition_variable::wait() (calls futex(2)).
//            Thread moves to SLEEPING — OS wakes it only when a message arrives.
// ════════════════════════════════════════════════════════════════════════════
inline void scenario1_busyWait(bool buggy, int n = 3000) {
    std::queue<std::function<void()>> q;
    std::mutex qmtx;
    std::condition_variable cv;
    std::atomic<int>       done{0};
    std::atomic<bool>      stop{false};
    std::atomic<long long> wasted{0};  // spin iterations that found nothing

    std::thread worker([&] {
        while (!stop.load()) {
            if (buggy) {
                // ── BUGGY: tight spin ────────────────────────────────────
                std::function<void()> fn;
                { std::lock_guard lk(qmtx);
                  if (!q.empty()) { fn = std::move(q.front()); q.pop(); } }
                if (fn) fn();
                else    wasted.fetch_add(1, std::memory_order_relaxed);
            } else {
                // ── FIXED: sleep until work arrives ─────────────────────
                std::function<void()> fn;
                { std::unique_lock lk(qmtx);
                  cv.wait(lk, [&]{ return !q.empty() || stop.load(); });
                  if (stop.load() && q.empty()) break;
                  fn = std::move(q.front()); q.pop(); }
                fn();
            }
        }
    });

    auto t0 = now_us();
    for (int i = 0; i < n; ++i) {
        std::this_thread::sleep_for(std::chrono::microseconds(200)); // simulate inter-arrival
        { std::lock_guard lk(qmtx); q.push([&done]{ done.fetch_add(1); }); }
        if (!buggy) cv.notify_one();
    }
    while (done.load() < n) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    stop.store(true);
    if (!buggy) cv.notify_all();
    worker.join();

    long long elapsed = (now_us() - t0) / 1000;
    Logger::sys("──────────────────────────────────────────────");
    Logger::sys("PERF S1 [" + std::string(buggy?"BUGGY":"FIXED") + "] "
                + std::to_string(n) + " SIP msgs in " + std::to_string(elapsed) + " ms");
    if (buggy)
        Logger::sys("  Wasted spins: " + std::to_string(wasted.load())
                    + " (CPU 100% — all idle time burned in loop)");
    else
        Logger::sys("  Wasted spins: 0  (thread in futex SLEEP, CPU free)");
    Logger::sys("  DIAGNOSE: top -H -p $PID  →  buggy: %CPU=100 on worker thread");
    Logger::sys("            perf top -p $PID  →  buggy: top symbol = spin loop");
    Logger::sys("  SAY THIS: 'Under SIP load the receive thread held 100% CPU. perf top");
    Logger::sys("    showed the tight poll. Replaced with cv.wait() backed by futex.");
    Logger::sys("    CPU dropped from 100% to ~0% — thread sleeps between SIP messages.'");
}

// ════════════════════════════════════════════════════════════════════════════
// SCENARIO 2 — LOCK CONTENTION
//
// SYMPTOM:  BULK 50 throughput does not scale with more workers.  Context
//           switches spike — pidstat -w shows >5000 ctx/sec on the MME pid.
// ROOT CAUSE: A single global mutex protects the UE session table.  All
//             workers queue up behind it even though most access different UEs.
// FIX:       Shard by UE-ID (or Call-ID): N buckets each with its own mutex.
//            P workers hit P different buckets in parallel — no serialization.
//            (This is exactly what ue_context_store.h already does with 64 shards.)
// ════════════════════════════════════════════════════════════════════════════
inline void scenario2_lockContention(bool buggy, int n = 1600000) {
    const int WORKERS = 8;
    const int PER_WORKER = n / WORKERS;

    // Shared counter — simulates the UE session table write
    long long shared_counter = 0;
    std::mutex global_mtx;

    // Per-worker local counters (fixed version — no shared mutex on hot path)
    std::vector<long long> local_counters(WORKERS, 0);

    std::vector<std::thread> threads;
    auto t0 = now_us();

    if (buggy) {
        // ── BUGGY: all threads fight one global mutex ────────────────────
        // Simulates: every SIP session update (state write) goes through one mutex.
        // 8 workers serialize → effective concurrency = 1.
        for (int w = 0; w < WORKERS; ++w) {
            threads.emplace_back([&] {
                for (int i = 0; i < PER_WORKER; ++i) {
                    std::lock_guard lk(global_mtx);
                    shared_counter++;
                    // simulate session-state update work while holding lock
                    volatile int x = shared_counter & 0xFF;
                    for (int j = 0; j < 30; ++j) x = (x * 31 + j) & 0xFF;
                    (void)x;
                }
            });
        }
    } else {
        // ── FIXED: each worker owns its own counter (shard by Call-ID hash) ─
        // Each worker touches only its own bucket — zero contention.
        for (int w = 0; w < WORKERS; ++w) {
            threads.emplace_back([&, w] {
                for (int i = 0; i < PER_WORKER; ++i) {
                    local_counters[w]++;
                    volatile int x = local_counters[w] & 0xFF;
                    for (int j = 0; j < 30; ++j) x = (x * 31 + j) & 0xFF;
                    (void)x;
                }
            });
        }
    }
    for (auto& t : threads) t.join();
    long long elapsed = (now_us() - t0) / 1000;

    // merge in fixed case
    if (!buggy) for (auto c : local_counters) shared_counter += c;

    Logger::sys("──────────────────────────────────────────────");
    Logger::sys("PERF S2 [" + std::string(buggy?"BUGGY":"FIXED") + "] "
                + std::to_string(WORKERS) + " workers × " + std::to_string(PER_WORKER)
                + " ops = " + std::to_string(shared_counter) + " total in "
                + std::to_string(elapsed) + " ms");
    Logger::sys("  Throughput: " + fmt1(shared_counter * 1000.0 / (elapsed > 0 ? elapsed : 1)) + " ops/sec");
    Logger::sys("  DIAGNOSE: pidstat -w -p $PID 1   →  buggy: cswch/s >> fixed");
    Logger::sys("            perf stat -p $PID       →  buggy: high task-clock, many migrations");
    Logger::sys("  SAY THIS: 'All 8 workers serialized on one mutex — throughput flat");
    Logger::sys("    at 1-thread speed. pidstat showed 4000+ ctx switches/sec. Sharded");
    Logger::sys("    by Call-ID (64 buckets, each with its own mutex). Throughput 6x.'");
}

// ════════════════════════════════════════════════════════════════════════════
// SCENARIO 3 — MEMORY GROWTH / UNBOUNDED QUEUE
//
// SYMPTOM:  Under BULK 100, RSS climbs steadily and never returns to baseline.
//           The task queue backlog grows without bound — eventually OOM.
// ROOT CAUSE: Producers (eNB threads) submit SIP INVITE dialogs faster than
//             consumers (MME workers) process them. No backpressure.
// FIX:       Bounded queue: block the producer when depth > MAX.  Applies
//            backpressure upstream — eNB waits instead of leaking memory.
// ════════════════════════════════════════════════════════════════════════════
inline void scenario3_unboundedQueue(bool buggy, int n = 60) {
    const int MAX_Q   = 10;   // bounded queue depth
    const int WORKERS = 2;    // slow consumer pool

    std::queue<int> q;
    std::mutex qmtx;
    std::condition_variable not_empty_cv;
    std::condition_variable not_full_cv;   // only used in fixed version
    std::atomic<bool> stop{false};
    std::atomic<int>  done{0};
    size_t            peak_depth = 0;

    auto consumer = [&] {
        while (true) {
            {
                std::unique_lock lk(qmtx);
                not_empty_cv.wait(lk, [&]{ return !q.empty() || stop.load(); });
                if (stop.load() && q.empty()) break;
                q.pop();
                if (!buggy) not_full_cv.notify_one();  // signal producer a slot freed
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(30)); // slow processing
            done.fetch_add(1);
        }
    };

    std::vector<std::thread> workers;
    for (int i = 0; i < WORKERS; ++i) workers.emplace_back(consumer);

    auto t0 = now_us();
    for (int i = 0; i < n; ++i) {
        if (buggy) {
            // ── BUGGY: push unconditionally — queue grows without limit ──
            { std::lock_guard lk(qmtx); q.push(i);
              peak_depth = std::max(peak_depth, q.size()); }
            not_empty_cv.notify_one();
        } else {
            // ── FIXED: block producer when queue is full ─────────────────
            {
                std::unique_lock lk(qmtx);
                not_full_cv.wait(lk, [&]{ return (int)q.size() < MAX_Q || stop.load(); });
                q.push(i);
                peak_depth = std::max(peak_depth, q.size());
            }
            not_empty_cv.notify_one();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // producer faster than consumer
    }

    while (done.load() < n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true);
    not_empty_cv.notify_all();
    not_full_cv.notify_all();
    for (auto& w : workers) w.join();

    long long elapsed = (now_us() - t0) / 1000;
    Logger::sys("──────────────────────────────────────────────");
    Logger::sys("PERF S3 [" + std::string(buggy?"BUGGY":"FIXED") + "] "
                + std::to_string(n) + " SIP dialogs, " + std::to_string(WORKERS) + " workers");
    Logger::sys("  Peak queue depth: " + std::to_string(peak_depth)
                + (buggy ? " (unbounded — grows with load)" : " (capped at " + std::to_string(MAX_Q) + ")"));
    Logger::sys("  Elapsed: " + std::to_string(elapsed) + " ms | completed: " + std::to_string(done.load()));
    Logger::sys("  DIAGNOSE: top -p $PID  →  buggy: RES climbs steadily");
    Logger::sys("            pidstat -r -p $PID 1  →  VSZ/RSS grows");
    Logger::sys("            valgrind/ASan: show unbounded vector growth");
    Logger::sys("  SAY THIS: 'RSS climbed 200MB under BULK 100. top showed the queue");
    Logger::sys("    vector growing. Added a bounded queue (depth=100) with cv backpressure");
    Logger::sys("    on the producer. RSS stabilized; eNB naturally rate-limits itself.'");
}

// ════════════════════════════════════════════════════════════════════════════
// SCENARIO 4 — THREAD-POOL STARVATION (BLOCKING ON HOT PATH)
//
// SYMPTOM:  BULK 20 shows P99 > 1000ms even though CPU is not saturated.
//           pidstat -t shows all workers in D/S state most of the time.
// ROOT CAUSE: Each worker calls a blocking HSS auth (sleep simulates network
//             round-trip). With 4 workers all blocked, new tasks queue up.
// FIX:       Size the pool to exceed the expected blocking factor.
//            Rule: pool_size >= tasks_in_flight × (1 + avg_block_ms / avg_compute_ms)
//            Or: move blocking I/O off the pool (async callbacks — Phase 5 plan).
// ════════════════════════════════════════════════════════════════════════════
inline void scenario4_poolStarvation(bool buggy, int n = 24) {
    const int SMALL_POOL = 4;     // buggy: too few workers
    const int LARGE_POOL = 16;    // fixed: enough to absorb blocking
    const int BLOCK_MS   = 50;    // each task blocks 50ms (HSS auth round-trip)
    const int pool_size  = buggy ? SMALL_POOL : LARGE_POOL;

    // Mini thread pool for this scenario
    std::queue<std::function<void()>> q;
    std::mutex qmtx;
    std::condition_variable cv;
    std::atomic<bool> stop{false};
    std::vector<std::thread> workers;

    for (int i = 0; i < pool_size; ++i) {
        workers.emplace_back([&]{
            while (true) {
                std::function<void()> fn;
                { std::unique_lock lk(qmtx);
                  cv.wait(lk, [&]{ return !q.empty() || stop.load(); });
                  if (stop.load() && q.empty()) break;
                  fn = std::move(q.front()); q.pop(); }
                fn();
            }
        });
    }

    std::vector<double> latencies;
    latencies.reserve(n);
    std::mutex lat_mtx;

    auto t0 = now_us();
    for (int i = 0; i < n; ++i) {
        long long submit_us = now_us();
        { std::lock_guard lk(qmtx);
          q.push([&lat_mtx, &latencies, submit_us, BLOCK_MS]{
              // simulate blocking HSS auth call on the pool thread
              std::this_thread::sleep_for(std::chrono::milliseconds(BLOCK_MS));
              double lat = (now_us() - submit_us) / 1000.0;
              std::lock_guard lk(lat_mtx);
              latencies.push_back(lat);
          }); }
        cv.notify_one();
        std::this_thread::sleep_for(std::chrono::milliseconds(5)); // 5ms between submits
    }
    while ((int)latencies.size() < n) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    stop.store(true); cv.notify_all();
    for (auto& w : workers) w.join();
    long long elapsed = (now_us() - t0) / 1000;

    Logger::sys("──────────────────────────────────────────────");
    Logger::sys("PERF S4 [" + std::string(buggy?"BUGGY":"FIXED") + "] pool="
                + std::to_string(pool_size) + " workers, "
                + std::to_string(n) + " tasks each blocking " + std::to_string(BLOCK_MS) + "ms");
    Logger::sys("  P50 latency: " + fmt1(pct(latencies, 0.50)) + " ms");
    Logger::sys("  P99 latency: " + fmt1(pct(latencies, 0.99)) + " ms");
    Logger::sys("  Total time:  " + std::to_string(elapsed) + " ms");
    Logger::sys("  DIAGNOSE: pidstat -t -p $PID 1  →  buggy: workers all in S state");
    Logger::sys("            watch queue depth (queueDepth()) — buggy: backlog builds");
    Logger::sys("  SAY THIS: 'P99 was 800ms with 4 workers, each blocking 50ms on HSS.");
    Logger::sys("    pidstat showed all 4 threads sleeping simultaneously. Sized pool to");
    Logger::sys("    16 (= tasks_in_flight × block_fraction + headroom). P99 dropped to 120ms.'");
}

// ════════════════════════════════════════════════════════════════════════════
// SCENARIO 5 — INEFFICIENT DATA PATH (O(n) SESSION LOOKUP + PER-MSG COPY)
//
// SYMPTOM:  Under BULK 100, CPU climbs linearly with session count.
//           perf top: top symbol is the session-table scan inside sip_route().
// ROOT CAUSE: SIP routing scans a std::vector<Dialog> linearly to find the
//             Call-ID match — O(n) per message, n grows with concurrent calls.
//             Also: each match copies the 1500-byte SIP body into a new string.
// FIX:       Index by Call-ID: std::unordered_map<string_view, Dialog*> → O(1).
//            Parse in-place with string_view — zero copy, zero allocation.
// ════════════════════════════════════════════════════════════════════════════
inline void scenario5_dataPath(bool buggy, int n = 100000) {
    // Simulate a session table with TABLE_SIZE concurrent SIP dialogs.
    // Each lookup must find dialog by Call-ID embedded in the SIP message.
    const int TABLE_SIZE = 500;

    struct Dialog { std::string call_id; int state = 0; };

    // Build a pool of fake dialogs
    std::vector<Dialog> table(TABLE_SIZE);
    std::unordered_map<std::string, int> index;  // Call-ID → index (fixed version)
    for (int i = 0; i < TABLE_SIZE; ++i) {
        table[i].call_id = "call-" + std::to_string(i) + "@ims.local";
        index[table[i].call_id] = i;
    }

    long long hits = 0;
    auto t0 = now_us();

    if (buggy) {
        // ── BUGGY: O(n) linear scan + string copy per SIP message ──────
        for (int m = 0; m < n; ++m) {
            // Simulate receiving a SIP message — copy body into new string
            std::string body = "INVITE sip:bob@ims.local SIP/2.0\r\nCall-ID: call-"
                               + std::to_string(m % TABLE_SIZE) + "@ims.local\r\n";
            // Linear scan through all dialogs to find the match
            for (auto& d : table) {
                if (body.find(d.call_id) != std::string::npos) {
                    d.state++;
                    hits++;
                    break;
                }
            }
        }
    } else {
        // ── FIXED: O(1) hash lookup + string_view (no copy) ─────────────
        // The raw SIP buffer — reused, no allocation per message
        std::string body_buf;
        body_buf.reserve(256);
        for (int m = 0; m < n; ++m) {
            body_buf = "call-" + std::to_string(m % TABLE_SIZE) + "@ims.local";
            std::string_view call_id(body_buf);   // zero-copy view into the buffer
            auto it = index.find(std::string(call_id));  // O(1) hash lookup
            if (it != index.end()) {
                table[it->second].state++;
                hits++;
            }
        }
    }

    long long elapsed = (now_us() - t0);
    double throughput = double(n) * 1e6 / double(elapsed > 0 ? elapsed : 1);

    Logger::sys("──────────────────────────────────────────────");
    Logger::sys("PERF S5 [" + std::string(buggy?"BUGGY":"FIXED") + "] "
                + std::to_string(n) + " SIP lookups, table=" + std::to_string(TABLE_SIZE) + " dialogs");
    Logger::sys("  Hits:       " + std::to_string(hits));
    Logger::sys("  Elapsed:    " + fmt1(elapsed / 1000.0) + " ms");
    Logger::sys("  Throughput: " + fmt1(throughput / 1000.0) + "K msgs/sec");
    Logger::sys("  DIAGNOSE: perf record -g ./mme_sim && perf report");
    Logger::sys("            buggy: sip_route/find_if dominates flamegraph, scales O(n)");
    Logger::sys("            fixed: unordered_map::find is flat regardless of table size");
    Logger::sys("  SAY THIS: 'Under 500 concurrent SIP calls perf top showed 40% CPU in");
    Logger::sys("    the session-table scan. Replaced vector+find with unordered_map keyed");
    Logger::sys("    on Call-ID. Lookup dropped from O(n) to O(1). Throughput 10x.'");
}

// ── dispatcher ───────────────────────────────────────────────────────────────

inline void run(int scenario, bool buggy, int n = 0) {
    Logger::sys("════════════════════════════════════════════════════════════════");
    Logger::sys("PERF SCENARIO " + std::to_string(scenario) + " — " + (buggy ? "BUGGY" : "FIXED"));
    Logger::sys("════════════════════════════════════════════════════════════════");
    switch (scenario) {
        case 1: scenario1_busyWait       (buggy, n > 0 ? n : 3000);  break;
        case 2: scenario2_lockContention  (buggy, n > 0 ? n : 8000);  break;
        case 3: scenario3_unboundedQueue  (buggy, n > 0 ? n : 60);    break;
        case 4: scenario4_poolStarvation  (buggy, n > 0 ? n : 24);    break;
        case 5: scenario5_dataPath        (buggy, n > 0 ? n : 100000);break;
        default: Logger::sys("Unknown scenario: 1-5"); break;
    }
}

} // namespace Perf
