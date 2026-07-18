#pragma once
// =============================================================================
// stats.h — Per-path statistics with reservoir sampling for latency percentiles
// =============================================================================

#include <atomic>
#include <cstdint>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <string>

namespace upf {

// =============================================================================
// PathStats — Thread-safe stats collector for a single forwarding path
//
// Reservoir sampling: instead of storing every latency value (impractical at
// millions of packets per second), we keep a fixed-size reservoir of 100,000
// samples. This gives statistically sound percentiles with bounded memory.
// =============================================================================
struct PathStats {
    static constexpr size_t RESERVOIR_SIZE = 100'000;

    std::atomic<uint64_t> pkts_rx{0};
    std::atomic<uint64_t> pkts_forwarded{0};
    std::atomic<uint64_t> pkts_dropped{0};
    std::atomic<uint64_t> pkts_no_pdr{0};

    // Latency reservoir: ring of nanosecond latency values.
    // We use a simple fill-then-wrap approach (not true random reservoir
    // sampling, but good enough for percentile estimation in benchmarks).
    std::vector<uint64_t> latencies;
    std::atomic<size_t>   sample_idx{0};

    PathStats() : latencies(RESERVOIR_SIZE, 0) {}

    // Record latency for a single packet (or a burst, using burst arrival time)
    void record_latency(uint64_t arrival_ns, uint64_t now_ns_val) {
        if (now_ns_val <= arrival_ns) return;  // clock skew guard
        uint64_t lat = now_ns_val - arrival_ns;
        size_t idx = sample_idx.fetch_add(1, std::memory_order_relaxed) % RESERVOIR_SIZE;
        latencies[idx] = lat;
    }

    struct Percentiles {
        uint64_t p50_ns{0};
        uint64_t p95_ns{0};
        uint64_t p99_ns{0};
        uint64_t avg_ns{0};
        uint64_t min_ns{0};
        uint64_t max_ns{0};
    };

    // compute_percentiles: sort the filled portion of the reservoir.
    // Called after the benchmark run completes (not in the hot path).
    Percentiles compute_percentiles() const {
        Percentiles p;
        size_t filled = std::min(sample_idx.load(std::memory_order_relaxed), RESERVOIR_SIZE);
        if (filled == 0) return p;

        std::vector<uint64_t> sorted(latencies.begin(), latencies.begin() + filled);
        std::sort(sorted.begin(), sorted.end());

        p.min_ns = sorted.front();
        p.max_ns = sorted.back();
        p.p50_ns = sorted[filled * 50 / 100];
        p.p95_ns = sorted[filled * 95 / 100];
        p.p99_ns = sorted[filled * 99 / 100];

        uint64_t sum = 0;
        for (auto v : sorted) sum += v;
        p.avg_ns = sum / filled;

        return p;
    }

    void reset() {
        pkts_rx.store(0);
        pkts_forwarded.store(0);
        pkts_dropped.store(0);
        pkts_no_pdr.store(0);
        sample_idx.store(0);
        std::fill(latencies.begin(), latencies.end(), 0);
    }
};

// =============================================================================
// print_comparison_table — Unicode box-drawing comparison table
// =============================================================================
inline void print_comparison_table(
    const std::string& label_a, const PathStats& stats_a, double dur_a_s,
    const std::string& label_b, const PathStats& stats_b, double dur_b_s)
{
    auto pa = stats_a.compute_percentiles();
    auto pb = stats_b.compute_percentiles();

    uint64_t total_a = stats_a.pkts_forwarded.load() + stats_a.pkts_dropped.load();
    uint64_t total_b = stats_b.pkts_forwarded.load() + stats_b.pkts_dropped.load();

    double mpps_a = (dur_a_s > 0) ? (double)total_a / dur_a_s / 1e6 : 0.0;
    double mpps_b = (dur_b_s > 0) ? (double)total_b / dur_b_s / 1e6 : 0.0;
    double speedup = (mpps_a > 0) ? mpps_b / mpps_a : 0.0;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════════╗\n");
    printf("║            FORWARDING PATH BENCHMARK — COMPARISON RESULTS           ║\n");
    printf("╠══════════════════════════╦═══════════════════╦═════════════════════╣\n");
    printf("║ Metric                   ║ %-17s ║ %-19s ║\n", label_a.c_str(), label_b.c_str());
    printf("╠══════════════════════════╬═══════════════════╬═════════════════════╣\n");
    printf("║ Packets processed        ║ %-17lu ║ %-19lu ║\n", (unsigned long)total_a, (unsigned long)total_b);
    printf("║ Throughput (Mpps)        ║ %-17.3f ║ %-19.3f ║\n", mpps_a, mpps_b);
    printf("║ Duration (s)             ║ %-17.2f ║ %-19.2f ║\n", dur_a_s, dur_b_s);
    printf("║ Forwarded                ║ %-17lu ║ %-19lu ║\n", (unsigned long)stats_a.pkts_forwarded.load(), (unsigned long)stats_b.pkts_forwarded.load());
    printf("║ Dropped                  ║ %-17lu ║ %-19lu ║\n", (unsigned long)stats_a.pkts_dropped.load(), (unsigned long)stats_b.pkts_dropped.load());
    printf("║ No-PDR drops             ║ %-17lu ║ %-19lu ║\n", (unsigned long)stats_a.pkts_no_pdr.load(), (unsigned long)stats_b.pkts_no_pdr.load());
    printf("╠══════════════════════════╬═══════════════════╬═════════════════════╣\n");
    printf("║ Latency avg (ns)         ║ %-17lu ║ %-19lu ║\n", (unsigned long)pa.avg_ns, (unsigned long)pb.avg_ns);
    printf("║ Latency p50 (ns)         ║ %-17lu ║ %-19lu ║\n", (unsigned long)pa.p50_ns, (unsigned long)pb.p50_ns);
    printf("║ Latency p95 (ns)         ║ %-17lu ║ %-19lu ║\n", (unsigned long)pa.p95_ns, (unsigned long)pb.p95_ns);
    printf("║ Latency p99 (ns)         ║ %-17lu ║ %-19lu ║\n", (unsigned long)pa.p99_ns, (unsigned long)pb.p99_ns);
    printf("║ Latency min  (ns)        ║ %-17lu ║ %-19lu ║\n", (unsigned long)pa.min_ns, (unsigned long)pb.min_ns);
    printf("║ Latency max  (ns)        ║ %-17lu ║ %-19lu ║\n", (unsigned long)pa.max_ns, (unsigned long)pb.max_ns);
    printf("╠══════════════════════════╩═══════════════════╩═════════════════════╣\n");
    printf("║ Speedup (%s / %s): %.2fx                           ║\n",
           label_b.c_str(), label_a.c_str(), speedup);
    printf("╚══════════════════════════════════════════════════════════════════════╝\n");
}

} // namespace upf
