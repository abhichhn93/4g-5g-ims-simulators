// =============================================================================
// upf_fastpath_demo.cpp — Interactive packet-by-packet trace demo
//
// Usage:
//   upf_fastpath_demo [--socket] [--dpdk] [--packets N] [--ues M]
//
// Default: runs both modes sequentially with 20 packets and 5 UEs.
// =============================================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <thread>
#include <chrono>
#include "packet.h"
#include "pfcp_rules.h"
#include "socket_path.h"
#include "dpdk_path.h"
#include "stats.h"

using namespace upf;
using namespace std::chrono_literals;

// ── Helpers ──────────────────────────────────────────────────────────────────

static std::string far_action_str(FarAction a) {
    switch (a) {
        case FarAction::FORWARD: return "FORWARD";
        case FarAction::DROP:    return "DROP";
        case FarAction::BUFFER:  return "BUFFER";
    }
    return "?";
}

static std::string qfi_name(uint8_t qfi) {
    if (qfi == 1) return "voice";
    if (qfi == 2) return "video";
    if (qfi == 9) return "data BE";
    return "unknown";
}

static std::string dir_str(Direction d) {
    return (d == Direction::UL) ? "UL " : "DL ";
}

// Generate a demo packet for UE index ue_idx, direction dir, sequence seq
static Packet make_demo_packet(uint32_t ue_idx, Direction dir, uint32_t seq, uint32_t num_ues) {
    Packet p;
    p.seq         = seq + 1;
    p.dir         = dir;
    p.teid        = 1000 + ue_idx;
    uint32_t base = (10u << 24) | (45u << 16) | (0u << 8) | 1u;
    p.ue_ip       = base + ue_idx;
    p.dscp        = (ue_idx % 3 == 0) ? 46 : (ue_idx % 3 == 1) ? 34 : 0;
    p.payload_len = (dir == Direction::UL) ? 60 : 1400;
    p.arrival_ns  = Packet::now_ns();
    p.qfi         = (ue_idx % 3 == 0) ? 1 : (ue_idx % 3 == 1) ? 2 : 9;
    (void)num_ues;
    return p;
}

// ── Socket-path demo ─────────────────────────────────────────────────────────

static void run_socket_demo(uint32_t num_packets, uint32_t num_ues, const RuleTable& rt) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║    MODE: Socket-Style Path (Linux kernel simulation)            ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    SocketPath sp(rt);
    sp.start();

    for (uint32_t i = 0; i < num_packets; ++i) {
        Direction dir = (i % 2 == 0) ? Direction::UL : Direction::DL;
        uint32_t  ue  = i % num_ues;
        Packet    pkt = make_demo_packet(ue, dir, i, num_ues);

        // Show what happens at each stage
        const PDR* pdr = rt.match_pdr(pkt);
        const FAR* far = pdr ? rt.get_far(pdr->far_id) : nullptr;
        const QER* qer = pdr ? rt.get_qer(pdr->qer_id) : nullptr;

        printf("[PKT #%04u] %s TEID=%-4u UE=%-12s DSCP=%-2u LEN=%u bytes\n",
               pkt.seq,
               dir_str(pkt.dir).c_str(),
               pkt.teid,
               pkt.ue_ip_str().c_str(),
               pkt.dscp,
               pkt.payload_len);

        printf("  -> [ENQUEUE]  mutex lock + queue push + cv.notify_one()\n");
        printf("  -> [WORKER ]  woke from cv::wait (futex unpark ~5-50us)\n");

        if (pdr) {
            printf("  -> [PDR    ]  matched PDR #%u (%s TEID=%u) -> FAR=%u, QER=%u\n",
                   pdr->pdr_id,
                   dir_str(pdr->dir).c_str(),
                   pdr->teid,
                   pdr->far_id,
                   pdr->qer_id);
        } else {
            printf("  -> [PDR    ]  NO MATCH — packet dropped (pkts_no_pdr++)\n");
        }

        if (qer) {
            printf("  -> [QER    ]  QFI=%u (%s), UL-MBR=%u kbps, DL-MBR=%u kbps\n",
                   qer->qfi,
                   qfi_name(qer->qfi).c_str(),
                   qer->ul_mbr_kbps,
                   qer->dl_mbr_kbps);
        }

        if (far) {
            printf("  -> [FAR    ]  action=%s -> dst=%u.%u.%u.%u %s\n",
                   far_action_str(far->action).c_str(),
                   (far->dst_ip >> 24) & 0xFF,
                   (far->dst_ip >> 16) & 0xFF,
                   (far->dst_ip >> 8)  & 0xFF,
                   (far->dst_ip)       & 0xFF,
                   far->encap_gtpu ? "(GTP-U encap, DL to gNB)" : "(N6 internet)");
        }

        uint64_t before = Packet::now_ns();
        sp.enqueue(pkt);

        // Give worker time to process for latency measurement
        std::this_thread::sleep_for(500us);

        uint64_t lat = Packet::now_ns() - before;
        printf("  -> [FORWARD]  observed latency ~%lu ns (incl. mutex+cv overhead)\n", (unsigned long)lat);
        printf("\n");
    }

    sp.stop();

    auto p = sp.stats().compute_percentiles();
    printf("─── Socket-path summary ───────────────────────────────────────────\n");
    printf("  Packets forwarded : %lu\n", (unsigned long)sp.stats().pkts_forwarded.load());
    printf("  Packets dropped   : %lu\n", (unsigned long)sp.stats().pkts_dropped.load());
    printf("  Latency avg       : %lu ns\n", (unsigned long)p.avg_ns);
    printf("  Latency p50       : %lu ns\n", (unsigned long)p.p50_ns);
    printf("  Latency p99       : %lu ns\n", (unsigned long)p.p99_ns);
    printf("───────────────────────────────────────────────────────────────────\n");
}

// ── DPDK-path demo ───────────────────────────────────────────────────────────

static void run_dpdk_demo(uint32_t num_packets, uint32_t num_ues, const RuleTable& rt) {
    printf("\n");
    printf("╔══════════════════════════════════════════════════════════════════╗\n");
    printf("║    MODE: DPDK-Style Path (poll + burst simulation)              ║\n");
    printf("╚══════════════════════════════════════════════════════════════════╝\n");
    printf("\n");

    DpdkPath dp(rt);
    dp.start();

    uint32_t burst_size  = 5;
    uint32_t num_bursts  = (num_packets + burst_size - 1) / burst_size;

    for (uint32_t b = 0; b < num_bursts; ++b) {
        uint32_t start = b * burst_size;
        uint32_t end   = std::min(start + burst_size, num_packets);
        uint32_t count = end - start;

        // Enqueue a batch of packets (simulates NIC RX filling the ring)
        printf("[PKT #%04u-%04u] rx_ring.push() x%u  (lock-free, no mutex)\n",
               start + 1, end, count);

        for (uint32_t i = start; i < end; ++i) {
            Direction dir = (i % 2 == 0) ? Direction::UL : Direction::DL;
            uint32_t  ue  = i % num_ues;
            Packet    pkt = make_demo_packet(ue, dir, i, num_ues);
            dp.rx_enqueue(pkt);
        }

        // Give lcore time to poll and process the burst
        std::this_thread::sleep_for(1ms);

        // Show burst processing detail
        printf("[BURST  #%03u] popped %u packets from RX ring (poll loop, no sleep)\n",
               b + 1, count);
        printf("  -> [PDR x%u]  batch PDR lookup (cache-warm, single now_ns() call)\n", count);
        printf("  -> [FAR x%u]  all FORWARD (if matched)\n", count);

        // Show individual packet results for first burst only
        if (b == 0) {
            for (uint32_t i = start; i < end; ++i) {
                Direction dir = (i % 2 == 0) ? Direction::UL : Direction::DL;
                uint32_t  ue  = i % num_ues;
                Packet    pkt = make_demo_packet(ue, dir, i, num_ues);
                const PDR* pdr = rt.match_pdr(pkt);
                const QER* qer = pdr ? rt.get_qer(pdr->qer_id) : nullptr;
                if (pdr && qer) {
                    printf("       PKT%u: %s UE=%s QFI=%u(%s)\n",
                           i + 1,
                           dir_str(dir).c_str(),
                           pkt.ue_ip_str().c_str(),
                           qer->qfi,
                           qfi_name(qer->qfi).c_str());
                }
            }
        }

        uint64_t burst_lat = dp.stats().compute_percentiles().avg_ns;
        printf("  -> [STAT   ]  %u forwarded, avg burst latency ~%lu ns\n",
               count, (unsigned long)burst_lat);
        printf("\n");
    }

    dp.stop();

    auto p = dp.stats().compute_percentiles();
    printf("─── DPDK-path summary ─────────────────────────────────────────────\n");
    printf("  Packets forwarded : %lu\n", (unsigned long)dp.stats().pkts_forwarded.load());
    printf("  Packets dropped   : %lu\n", (unsigned long)dp.stats().pkts_dropped.load());
    printf("  Latency avg       : %lu ns\n", (unsigned long)p.avg_ns);
    printf("  Latency p50       : %lu ns\n", (unsigned long)p.p50_ns);
    printf("  Latency p99       : %lu ns\n", (unsigned long)p.p99_ns);
    printf("───────────────────────────────────────────────────────────────────\n");
}

// ── Main ─────────────────────────────────────────────────────────────────────

int main(int argc, char* argv[]) {
    bool run_socket = false;
    bool run_dpdk   = false;
    uint32_t num_packets = 20;
    uint32_t num_ues     = 5;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--socket") == 0) { run_socket = true; }
        else if (strcmp(argv[i], "--dpdk") == 0) { run_dpdk = true; }
        else if (strcmp(argv[i], "--packets") == 0 && i + 1 < argc) {
            num_packets = (uint32_t)atoi(argv[++i]);
        }
        else if (strcmp(argv[i], "--ues") == 0 && i + 1 < argc) {
            num_ues = (uint32_t)atoi(argv[++i]);
        }
    }

    // Default: run both
    if (!run_socket && !run_dpdk) {
        run_socket = true;
        run_dpdk   = true;
    }

    // ── Educational header ────────────────────────────────────────────────
    printf("\n");
    printf("================================================================\n");
    printf("  UPF FAST-PATH LAB — Socket vs DPDK Forwarding Path Demo\n");
    printf("================================================================\n");
    printf("\n");
    printf("WHAT THIS DEMONSTRATES:\n");
    printf("  Socket path : Models a Linux kernel socket-based UPF.\n");
    printf("                Each packet goes through: syscall -> kernel copy ->\n");
    printf("                mutex -> condition_variable wake -> process.\n");
    printf("                Overhead: 5-50us per packet (dominated by CV wake).\n");
    printf("\n");
    printf("  DPDK path   : Models a DPDK poll-mode UPF.\n");
    printf("                Each packet goes through: lock-free ring -> burst ->\n");
    printf("                process (no mutex, no sleep, no syscall).\n");
    printf("                Overhead: 50-500ns per packet.\n");
    printf("\n");
    printf("PFCP RULES:\n");
    printf("  %u UEs simulated. Each has:\n", num_ues);
    printf("    - UL PDR: match TEID (from gNB GTP-U) -> FAR: forward to N6\n");
    printf("    - DL PDR: match UE-IP (from internet)  -> FAR: GTP-U encap to gNB\n");
    printf("    - QER: QFI cycles voice(1)/video(2)/data(9) with MBR enforcement\n");
    printf("\n");
    printf("  Packets:  %u\n", num_packets);
    printf("  UEs:      %u\n", num_ues);
    printf("================================================================\n");

    // Build rule table once (shared by both paths)
    RuleTable rt = build_demo_rules(num_ues);

    if (run_socket) {
        run_socket_demo(num_packets, num_ues, rt);
    }

    if (run_dpdk) {
        run_dpdk_demo(num_packets, num_ues, rt);
    }

    printf("\n");
    printf("================================================================\n");
    printf("  Run 'upf_benchmark' for throughput/latency comparison.\n");
    printf("================================================================\n");
    printf("\n");

    return 0;
}
