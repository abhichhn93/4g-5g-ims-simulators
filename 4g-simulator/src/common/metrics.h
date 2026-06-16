#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <numeric>
#include <string>
#include <vector>
#include "common/logger.h"

// ============================================================
// METRICS — per-attach latency tracking + P95/P99 reporting
//
// WHAT WE MEASURE:
//   Attach latency = time from InitialUEMessage received at MME
//                     to Attach Complete received at MME
//   This includes: auth (HSS round trip) + session (S-GW/P-GW/PCRF) + bearer setup
//
// WHY P95/P99?
//   Mean is misleading when there are outliers.
//   P95 = 95th percentile: 95% of attaches completed FASTER than this.
//   P99 = 99th percentile: used for SLA — "99% of attaches < 500ms"
//   Samsung SLA: P99 attach < 2 seconds under 10K/min load.
//
// INTERVIEW Q: "How would you detect a regression in attach latency?"
// ANSWER: "Prometheus histogram metrics per MME pod. Alert if P99 crosses
//   SLA threshold. Track per-step latency (auth vs session vs bearer) to
//   identify the bottleneck. Grafana dashboard for real-time trending."
//
// CLOUD-NATIVE:
//   Real systems export metrics via /metrics (Prometheus scrape endpoint)
//   Each AMF pod exposes: attach_duration_seconds histogram
//   Kubernetes: HPA scales pods when P95 > threshold or CPU > 70%
// ============================================================
class Metrics {
public:
    // Call before starting BULK N — resets state for a new batch
    void startBulk(int expected_count) {
        std::lock_guard<std::mutex> lk(mutex_);
        latencies_ms_.clear();
        latencies_ms_.reserve(expected_count);
        expected_ = expected_count;
        completed_.store(0);
        bulk_start_ = std::chrono::steady_clock::now();
    }

    // Called by MME when a UE reaches REGISTERED state
    // Thread-safe: multiple workers may complete simultaneously
    void recordAttach(double latency_ms) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            latencies_ms_.push_back(latency_ms);
        }
        // If all expected attaches complete, wake waitForAll()
        if (completed_.fetch_add(1) + 1 >= expected_) {
            done_cv_.notify_all();
        }
    }

    // Block until all expected attaches complete or timeout
    bool waitForAll(int timeout_ms) {
        std::unique_lock<std::mutex> lk(done_mutex_);
        return done_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                                  [this]{ return completed_.load() >= expected_; });
    }

    int completedCount() const { return completed_.load(); }

    // Prometheus text exposition format (TS: scrape every 15s by default)
    // Format: https://prometheus.io/docs/instrumenting/exposition_formats/
    std::string prometheusText() const {
        std::lock_guard<std::mutex> lk(mutex_);
        std::string out;
        out.reserve(1024);

        int n = int(latencies_ms_.size());
        double avg_ms = 0, p50 = 0, p95 = 0, p99 = 0, mn = 0, mx = 0, tput = 0;
        if (n > 0) {
            auto sorted = latencies_ms_;
            std::sort(sorted.begin(), sorted.end());
            double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
            avg_ms = sum / n;
            p50 = percentile(sorted, 0.50);
            p95 = percentile(sorted, 0.95);
            p99 = percentile(sorted, 0.99);
            mn  = sorted.front();
            mx  = sorted.back();
            auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - bulk_start_).count();
            tput = n * 1000.0 / double(total_ms > 0 ? total_ms : 1);
        }

        auto line = [&](const char* name, const char* help, const char* type,
                        double value, const char* labels = "") {
            out += "# HELP "; out += name; out += ' '; out += help; out += '\n';
            out += "# TYPE "; out += name; out += ' '; out += type; out += '\n';
            out += name;
            if (labels && labels[0]) { out += '{'; out += labels; out += '}'; }
            out += ' '; out += fmt(value); out += '\n';
        };

        line("mme_attach_total", "Total UE attaches recorded", "counter", double(n));
        line("mme_attach_latency_ms", "Attach latency mean (ms)", "gauge", avg_ms, "quantile=\"mean\"");
        line("mme_attach_latency_ms", "Attach latency P50 (ms)", "gauge", p50, "quantile=\"0.50\"");
        line("mme_attach_latency_ms", "Attach latency P95 (ms)", "gauge", p95, "quantile=\"0.95\"");
        line("mme_attach_latency_ms", "Attach latency P99 (ms)", "gauge", p99, "quantile=\"0.99\"");
        line("mme_attach_latency_ms_min", "Attach latency min (ms)", "gauge", mn);
        line("mme_attach_latency_ms_max", "Attach latency max (ms)", "gauge", mx);
        line("mme_attach_throughput", "Attaches per second (recent bulk)", "gauge", tput);
        line("mme_expected_ues", "Expected UEs in current batch", "gauge", double(expected_));
        line("mme_registered_ues", "UEs that completed attach", "gauge", double(completed_.load()));
        return out;
    }

    void printReport() const {
        std::lock_guard<std::mutex> lk(mutex_);
        if (latencies_ms_.empty()) {
            Logger::sys("METRICS: no data recorded");
            return;
        }

        auto sorted = latencies_ms_;
        std::sort(sorted.begin(), sorted.end());

        double sum = std::accumulate(sorted.begin(), sorted.end(), 0.0);
        double avg = sum / double(sorted.size());
        double p50 = percentile(sorted, 0.50);
        double p95 = percentile(sorted, 0.95);
        double p99 = percentile(sorted, 0.99);
        double mn  = sorted.front();
        double mx  = sorted.back();

        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - bulk_start_).count();
        double throughput = sorted.size() * 1000.0 / double(total_ms > 0 ? total_ms : 1);

        Logger::sys("════════════════════════════════════════════════");
        Logger::sys("BULK METRICS — " + std::to_string(sorted.size()) + " UEs completed");
        Logger::sys("  Latency avg:   " + fmt(avg)  + " ms");
        Logger::sys("  Latency P50:   " + fmt(p50)  + " ms  (median)");
        Logger::sys("  Latency P95:   " + fmt(p95)  + " ms  (SLA target: < 2000ms)");
        Logger::sys("  Latency P99:   " + fmt(p99)  + " ms");
        Logger::sys("  Min / Max:     " + fmt(mn) + " / " + fmt(mx) + " ms");
        Logger::sys("  Total time:    " + std::to_string(total_ms) + " ms");
        Logger::sys("  Throughput:    " + fmt(throughput) + " attaches/sec");
        Logger::sys("────────────────────────────────────────────────");
        Logger::sys("  INTERVIEW: 'What's your P99 attach time?' → show this.");
        Logger::sys("  NEXT STEP: Phase 5 async MME → parallel auth → 10x throughput");
        Logger::sys("════════════════════════════════════════════════");
    }

private:
    mutable std::mutex            mutex_;
    std::vector<double>           latencies_ms_;
    std::atomic<int>              completed_{0};
    int                           expected_{0};
    std::chrono::steady_clock::time_point bulk_start_;

    std::condition_variable done_cv_;
    std::mutex              done_mutex_;

    static double percentile(const std::vector<double>& sorted, double p) {
        if (sorted.empty()) return 0;
        size_t idx = size_t(p * double(sorted.size()));
        if (idx >= sorted.size()) idx = sorted.size() - 1;
        return sorted[idx];
    }

    static std::string fmt(double v) {
        char buf[32]; std::snprintf(buf, sizeof(buf), "%.1f", v); return buf;
    }
};
