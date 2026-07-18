#pragma once
// =============================================================================
// packet.h — Core packet descriptor for UPF fast-path simulation
//
// In a real UPF (e.g., OAI-UPF, free5GC UPF, VPP-UPF):
//   - UL packets arrive on N3 (gNB → UPF) as GTP-U encapsulated UDP/IP.
//     The UPF strips the GTP-U header, extracts the inner IP packet,
//     classifies it via PDR, applies QoS, and forwards to N6 (internet).
//   - DL packets arrive on N6 (internet → UPF) as plain IP.
//     The UPF looks up the UE IP, finds the PDR/FAR, adds a GTP-U header,
//     and sends to the gNB tunnel on N3.
//
// In DPDK UPFs, the "packet descriptor" is an rte_mbuf. We simulate the
// metadata fields that matter for classification and QoS.
// =============================================================================

#include <cstdint>
#include <chrono>
#include <string>
#include <arpa/inet.h>

namespace upf {

// Direction of packet flow relative to the UE.
enum class Direction : uint8_t {
    UL = 0,   // Uplink: gNB → UPF → internet (N3 → N6)
    DL = 1,   // Downlink: internet → UPF → gNB (N6 → N3)
};

struct Packet {
    // ── GTP-U tunnel identifier ───────────────────────────────────────────
    // TEID (Tunnel Endpoint Identifier): 32-bit value assigned by the gNB
    // during session setup via N2 (NGAP). Every UL GTP-U packet from the gNB
    // carries this TEID. The UPF uses it as the primary UL classification key
    // in the PDR (Packet Detection Rule).
    uint32_t teid{0};

    // ── UE IP address (inner packet) ─────────────────────────────────────
    // The IP address assigned to the UE's PDU session (e.g., 10.45.0.1).
    // For DL packets arriving on N6, the UPF matches the destination IP
    // against this field to find the correct PDR/FAR and GTP-U tunnel.
    uint32_t ue_ip{0};          // host byte order

    // ── DSCP (Differentiated Services Code Point) ────────────────────────
    // 6-bit value in the IP header TOS/Traffic Class field.
    // In 5G, the SMF maps 5QI → DSCP when programming the FAR.
    // DSCP 46 = EF (Expedited Forwarding) → voice
    // DSCP 34 = AF41 → video streaming
    // DSCP 0  = BE (Best Effort) → data
    uint8_t dscp{0};

    // ── Payload length (inner IP packet) ─────────────────────────────────
    // Bytes of the inner PDU. In real DPDK: rte_mbuf->pkt_len.
    // Typical values: ~60-200 bytes (VoIP), 1400 bytes (video/data).
    uint16_t payload_len{0};

    // ── Arrival timestamp (nanoseconds) ──────────────────────────────────
    // Set by the NIC hardware in production DPDK (hardware timestamping)
    // or by PMD software before passing to application.
    // We use std::chrono::high_resolution_clock here.
    // Used to measure per-packet latency = (forward_ns - arrival_ns).
    uint64_t arrival_ns{0};

    // ── QFI (QoS Flow Identifier) ────────────────────────────────────────
    // 6-bit value identifying the QoS flow within the PDU session.
    // Carried in the GTP-U extension header (PDU Session Container) on N3.
    // The QER (QoS Enforcement Rule) is keyed by QFI.
    // Common values: 1=voice (GBR), 2=video (GBR), 9=default data (non-GBR)
    uint8_t qfi{0};

    // ── Rule IDs (filled in by classifier) ───────────────────────────────
    // After PDR lookup, these are populated so downstream pipeline stages
    // (QER enforcer, FAR forwarder) can find the right rules without
    // re-doing the hash lookup.
    uint32_t pdr_id{0};   // matched PDR identifier
    uint32_t far_id{0};   // FAR to apply (from the matched PDR)

    // ── Classification and action flags ──────────────────────────────────
    bool classified{false};  // true after PDR lookup succeeded
    bool dropped{false};     // true if FAR action = DROP or no PDR matched

    // ── Sequence number (for demo tracing) ───────────────────────────────
    // Not present in real packets; used by the demo to label output lines.
    uint32_t seq{0};

    // Direction of this packet
    Direction dir{Direction::UL};

    // ── Static helpers ────────────────────────────────────────────────────

    // Returns current monotonic time in nanoseconds.
    // In real DPDK: rte_get_tsc_cycles() / rte_get_tsc_hz() * 1e9,
    // or hardware NIC timestamp from rte_mbuf->ol_flags.
    static uint64_t now_ns() {
        using clock = std::chrono::high_resolution_clock;
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock::now().time_since_epoch()).count());
    }

    // Human-readable UE IP (e.g., "10.45.0.3")
    std::string ue_ip_str() const {
        struct in_addr addr{};
        addr.s_addr = htonl(ue_ip);
        char buf[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr, buf, sizeof(buf));
        return std::string(buf);
    }
};

} // namespace upf
