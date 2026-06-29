#pragma once
// ============================================================
// IMS PERFORMANCE SCENARIOS — 5 Classic Interview Problems
//
// Each scenario has:
//   - BUGGY version: demonstrates the performance problem
//   - FIXED version: shows the solution
//   - Toggle via SCENARIO_* environment variables
//   - Metrics printed before/after
// ============================================================

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstring>

namespace PerfScenarios {

// ── Common Types ──────────────────────────────────────────────
struct SipMessage {
    std::string call_id;
    std::string from_impu;
    std::string to_impu;
    std::string body;  // SIP body (SDP etc.)
    uint32_t seq{0};
    uint64_t timestamp_us{0};
};

struct CallSession {
    std::string call_id;
    std::string caller;
    std::string callee;
    std::string state;  // "calling", "ringing", "active", "ended"
    uint64_t start_time_us{0};
};

// ── Metrics Collector ─────────────────────────────────────────
class Metrics {
public:
    void recordCallStart() {
        std::lock_guard<std::mutex> lk(mtx_);
        calls_started_++;
    }
    void recordCallEnd(uint64_t duration_us) {
        std::lock_guard<std::mutex> lk(mtx_);
        calls_completed_++;
        total_duration_us_ += duration_us;
        if (duration_us > max_duration_us_) max_duration_us_ = duration_us;
    }
    void recordCpuSample(double pct) {
        std::lock_guard<std::mutex> lk(mtx_);
        cpu_samples_.push_back(pct);
    }
    void recordMemoryMB(size_t mb) {
        std::lock_guard<std::mutex> lk(mtx_);
        mem_samples_.push_back(mb);
    }

    struct Stats {
        uint64_t calls_started{0};
        uint64_t calls_completed{0};
        uint64_t avg_duration_us{0};
        uint64_t max_duration_us{0};
        double avg_cpu_pct{0};
        double max_cpu_pct{0};
        size_t avg_mem_mb{0};
        size_t max_mem_mb{0};
        double throughput{0};  // calls/sec
    };

    Stats getStats() {
        std::lock_guard<std::mutex> lk(mtx_);
        Stats s;
        s.calls_started = calls_started_;
        s.calls_completed = calls_completed_;
        s.max_duration_us = max_duration_us_;
        if (calls_completed_ > 0) {
            s.avg_duration_us = total_duration_us_ / calls_completed_;
        }
        if (!cpu_samples_.empty()) {
            double sum = 0;
            for (double c : cpu_samples_) { sum += c; if (c > s.max_cpu_pct) s.max_cpu_pct = c; }
            s.avg_cpu_pct = sum / cpu_samples_.size();
        }
        if (!mem_samples_.empty()) {
            size_t sum = 0;
            for (size_t m : mem_samples_) { sum += m; if (m > s.max_mem_mb) s.max_mem_mb = m; }
            s.avg_mem_mb = sum / mem_samples_.size();
        }
        if (calls_completed_ > 0 && !cpu_samples_.empty()) {
            // Rough throughput estimate
            s.throughput = calls_completed_ * 1000.0 / (cpu_samples_.size() * 100); // assuming 100ms sampling
        }
        return s;
    }

    void reset() {
        std::lock_guard<std::mutex> lk(mtx_);
        calls_started_ = 0;
        calls_completed_ = 0;
        total_duration_us_ = 0;
        max_duration_us_ = 0;
        cpu_samples_.clear();
        mem_samples_.clear();
    }

private:
    std::mutex mtx_;
    uint64_t calls_started_{0};
    uint64_t calls_completed_{0};
    uint64_t total_duration_us_{0};
    uint64_t max_duration_us_{0};
    std::vector<double> cpu_samples_;
    std::vector<size_t> mem_samples_;
};

extern Metrics g_metrics;

// ── Scenario 1: CPU 100% — Busy Wait vs Condition Variable ────
//
// BUG: Polling a queue with no backoff (busy-wait loop)
// FIX: Use condition_variable to block until data arrives
//
class Scenario1_QueueProcessing {
public:
    Scenario1_QueueProcessing(bool buggy = true);
    ~Scenario1_QueueProcessing();

    void enqueue(const SipMessage& msg);
    void run(int duration_ms);
    void stop();

    // Metrics
    uint64_t getMessagesProcessed() const { return messages_processed_.load(); }
    double getAvgCpuPct() const { return avg_cpu_pct_.load(); }

private:
    void workerLoop();

    bool buggy_;
    std::atomic<bool> running_{false};
    std::queue<SipMessage> queue_;
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;  // Used only in FIXED version
    std::atomic<uint64_t> messages_processed_{0};
    std::atomic<double> avg_cpu_pct_{0};
    std::thread worker_;
};

// ── Scenario 2: Lock Contention — Global Mutex vs Sharding ────
//
// BUG: One global mutex serializing all worker threads
// FIX: Shard state by Call-ID hash (per-shard mutex)
//
class Scenario2_LockContention {
public:
    static constexpr int NUM_SHARDS = 16;

    Scenario2_LockContention(bool buggy = true);
    ~Scenario2_LockContention();

    void processCall(const std::string& call_id, const std::string& data);
    void run(int num_calls, int num_workers);
    void stop();

    uint64_t getCallsProcessed() const { return calls_processed_.load(); }
    uint64_t getContentions() const { return contentions_.load(); }
    uint64_t getAvgLatencyUs() const { return avg_latency_us_.load(); }

private:
    struct SessionTableBuggy {
        std::mutex global_mtx;
        std::unordered_map<std::string, CallSession> sessions;
    };

    struct SessionShard {
        std::mutex mtx;
        std::unordered_map<std::string, CallSession> sessions;
    };

    bool buggy_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> calls_processed_{0};
    std::atomic<uint64_t> contentions_{0};
    std::atomic<uint64_t> avg_latency_us_{0};

    // BUGGY: single global mutex
    std::unique_ptr<SessionTableBuggy> buggy_table_;

    // FIXED: sharded by call_id hash
    std::vector<std::unique_ptr<SessionShard>> shards_;

    std::vector<std::thread> workers_;
};

// ── Scenario 3: Memory Growth — Leak vs RAII ──────────────────
//
// BUG: SIP dialogs not freed on error paths, unbounded queue
// FIX: RAII/smart pointers + bounded queue with backpressure
//
class Scenario3_MemoryLeak {
public:
    Scenario3_MemoryLeak(bool buggy = true);
    ~Scenario3_MemoryLeak();

    void createDialog(const std::string& call_id);
    void simulateError(const std::string& call_id);
    void run(int num_operations);
    void stop();

    size_t getActiveDialogs() const;
    size_t getQueuedMessages() const;
    size_t getMemoryUsageKB() const;

private:
    struct DialogContext {
        std::string call_id;
        std::string state;
        // BUGGY: raw pointer to body (leaked on error)
        // FIXED: unique_ptr (auto-cleanup)
        char* body_buggy;         // raw - will leak
        std::unique_ptr<std::string> body_fixed;
    };

    bool buggy_;
    std::atomic<bool> running_{false};

    std::mutex dialogs_mtx_;
    std::vector<DialogContext*> dialogs_;  // BUGGY: raw pointers
    std::vector<std::unique_ptr<DialogContext>> safe_dialogs_;  // FIXED: unique_ptr

    std::queue<SipMessage> message_queue_;
    std::mutex queue_mtx_;
    // BUGGY: no bound on queue size
    // FIXED: bounded queue

    std::atomic<size_t> memory_kb_{0};
    std::thread worker_;
};

// ── Scenario 4: Thread Pool Starvation — Blocking on Hot Path ─
//
// BUG: Blocking I/O on event loop thread, too few workers
// FIX: Move blocking work off hot path, size pool to cores
//
class Scenario4_ThreadStarvation {
public:
    Scenario4_ThreadStarvation(bool buggy = true);
    ~Scenario4_ThreadStarvation();

    void submitWork(const std::function<void()>& task);
    void run(int num_tasks);
    void stop();

    uint64_t getTasksCompleted() const { return tasks_completed_.load(); }
    uint64_t getAvgLatencyUs() const { return avg_latency_us_.load(); }
    uint64_t getTimeouts() const { return timeouts_.load(); }

private:
    bool buggy_;
    std::atomic<bool> running_{false};
    std::queue<std::function<void()>> task_queue_;
    std::mutex queue_mtx_;
    std::condition_variable queue_cv_;

    std::atomic<uint64_t> tasks_completed_{0};
    std::atomic<uint64_t> avg_latency_us_{0};
    std::atomic<uint64_t> timeouts_{0};

    std::vector<std::thread> workers_;
};

// ── Scenario 5: Inefficient Data Path — O(n) lookup + copies ──
//
// BUG: O(n) session table scan, per-message string copies
// FIX: Hash-indexed session table, zero-copy parsing (string_view)
//
class Scenario5_InefficientDataPath {
public:
    Scenario5_InefficientDataPath(bool buggy = true);
    ~Scenario5_InefficientDataPath();

    void processMessage(const SipMessage& msg);
    void run(int num_messages);
    void stop();

    uint64_t getMessagesProcessed() const { return messages_processed_.load(); }
    uint64_t getAvgLatencyUs() const { return avg_latency_us_.load(); }
    uint64_t getAllocations() const { return allocations_.load(); }

private:
    // BUGGY: vector-based O(n) lookup
    struct SessionListBuggy {
        std::vector<CallSession> sessions;
        CallSession* find(const std::string& call_id) {
            for (auto& s : sessions) {
                if (s.call_id == call_id) return &s;
            }
            return nullptr;
        }
    };

    // FIXED: hash-indexed O(1) lookup
    struct SessionMapFixed {
        std::unordered_map<std::string, CallSession> sessions;
        CallSession* find(const std::string& call_id) {
            auto it = sessions.find(call_id);
            return it != sessions.end() ? &it->second : nullptr;
        }
        CallSession& operator[](const std::string& call_id) {
            return sessions[call_id];
        }
    };

    bool buggy_;
    std::atomic<bool> running_{false};

    std::unique_ptr<SessionListBuggy> buggy_sessions_;
    std::unique_ptr<SessionMapFixed> fixed_sessions_;

    std::atomic<uint64_t> messages_processed_{0};
    std::atomic<uint64_t> avg_latency_us_{0};
    std::atomic<uint64_t> allocations_{0};

    std::thread worker_;
};

// ── Load Generator ────────────────────────────────────────────
// Reuses the existing bulk-call pattern from the IMS simulator
class LoadGenerator {
public:
    LoadGenerator();
    ~LoadGenerator();

    // Configure load
    void setCallRate(int calls_per_second) { call_rate_ = calls_per_second; }
    void setNumUes(int n) { num_ues_ = n; }
    void setCallDurationMs(int ms) { call_duration_ms_ = ms; }

    // Run load test
    void run(int duration_seconds);
    void stop();

    bool isRunning() const { return running_.load(); }

private:
    void generateCalls();

    std::atomic<bool> running_{false};
    int call_rate_{100};       // calls per second
    int num_ues_{10};          // number of UEs
    int call_duration_ms_{2000}; // average call duration
    std::thread generator_;
};

// ── Scenario Runner ───────────────────────────────────────────
class ScenarioRunner {
public:
    static ScenarioRunner& instance();

    // Run a specific scenario in buggy mode, then fixed mode
    void runScenario1(int duration_ms = 5000);
    void runScenario2(int num_calls = 10000, int num_workers = 4);
    void runScenario3(int num_operations = 10000);
    void runScenario4(int num_tasks = 1000);
    void runScenario5(int num_messages = 10000);

    // Run all scenarios
    void runAll();

    // Print results
    void printResults();

    // Control which version to run: setMode(run_fixed, run_buggy)
    void setMode(bool run_fixed, bool run_buggy) {
        run_fixed_ = run_fixed;
        run_buggy_ = run_buggy;
    }

private:
    ScenarioRunner() = default;
    bool run_buggy_{true};
    bool run_fixed_{true};
    struct Result {
        std::string name;
        std::string metric_name;
        double buggy_value;
        double fixed_value;
        std::string unit;
    };
    std::vector<Result> results_;
};

} // namespace PerfScenarios