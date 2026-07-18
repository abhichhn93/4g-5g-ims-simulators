#pragma once
// =============================================================================
// pfcp_rules.h — PDR, FAR, QER rule tables (PFCP data model)
//
// PFCP (Packet Forwarding Control Protocol, 3GPP TS 29.244) is the N4 protocol
// between SMF and UPF. When a UE sets up a PDU session:
//
//   1. AMF → SMF: PDU Session Establishment request (N11)
//   2. SMF → UDM: subscriber profile fetch (N10)
//   3. SMF → UPF: PFCP Session Establishment Request (N4)
//        - Contains: list of PDRs, FARs, QERs for this UE session
//   4. UPF: installs rules into fast-path tables
//   5. SMF → gNB (via AMF): N2 PDU Session Resource Setup (allocates TEID)
//
// After this, every packet the UPF receives is classified by the fast-path:
//   Packet → match PDR → get FAR + QER → apply QoS → forward
//
// In DPDK UPFs, these tables are often implemented as:
//   - rte_hash (cuckoo hash) for TEID/UE-IP → PDR lookup
//   - Array indexed by rule ID for FAR/QER (O(1), cache-friendly)
// =============================================================================

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>
#include "packet.h"

namespace upf {

// =============================================================================
// PDR — Packet Detection Rule (3GPP TS 29.244 §7.5.2.2)
//
// A PDR defines how to classify an incoming packet. The UPF evaluates all PDRs
// in precedence order and applies the first match. In practice, well-designed
// implementations build hash tables keyed by TEID (UL) and UE-IP (DL) so that
// lookup is always O(1) without linear search.
// =============================================================================
struct PDR {
    uint32_t  pdr_id{0};       // Unique identifier within the PFCP session
    uint32_t  teid{0};         // UL match key: GTP-U TEID from the N3 header
    uint32_t  ue_ip{0};        // DL match key: inner destination IP (host order)
    Direction dir{Direction::UL};
    uint32_t  far_id{0};       // Which FAR to apply when this PDR matches
    uint32_t  qer_id{0};       // Which QER to apply (QoS enforcement)
    uint32_t  precedence{100}; // Lower number = higher priority (evaluated first)
};

// =============================================================================
// FAR — Forwarding Action Rule (3GPP TS 29.244 §7.5.2.3)
//
// A FAR defines what to do with a packet after PDR match:
//   FORWARD  → send to next hop (N6 internet for UL, N3 gNB for DL)
//   DROP     → discard (used for policy enforcement, idle UE, RAT change)
//   BUFFER   → buffer in UPF while gNB is unavailable (handover scenario)
//
// For UL FORWARD: dst_ip/dst_port = N6 next-hop (internet gateway)
// For DL FORWARD: encap_gtpu=true, dst_ip = gNB N3 IP, dst_port = 2152
// =============================================================================
enum class FarAction : uint8_t {
    FORWARD = 0,
    DROP    = 1,
    BUFFER  = 2,  // Hold packets during handover / paging
};

struct FAR {
    uint32_t   far_id{0};
    FarAction  action{FarAction::FORWARD};
    uint32_t   dst_ip{0};       // Next-hop IP (host byte order)
    uint16_t   dst_port{0};     // Next-hop port (2152 for GTP-U DL, 0 for UL)
    bool       encap_gtpu{false}; // true for DL: wrap inner IP in GTP-U
};

// =============================================================================
// QER — QoS Enforcement Rule (3GPP TS 29.244 §7.5.2.6)
//
// QER enforces the QoS profile for a UE's QoS flow. The SMF derives MBR/GBR
// values from the subscriber profile (UDM) and the requested QoS parameters.
//
// MBR (Maximum Bit Rate): hard cap — packets exceeding MBR are dropped.
// GBR (Guaranteed Bit Rate): minimum guaranteed — for GBR QoS flows (voice,
//   real-time video). The UPF must reserve resources for GBR flows.
// Non-GBR flows (QFI=9, best-effort data) have no GBR, only MBR.
//
// In a real DPDK UPF, QER is implemented as a token-bucket policer using
// rte_meter_trtcm (Two Rate Three Color Marker per RFC 4115).
// =============================================================================
struct QER {
    uint32_t qer_id{0};
    uint8_t  qfi{0};           // QoS Flow Identifier (0-63)
    uint32_t ul_mbr_kbps{0};  // UL Maximum Bit Rate (kbps)
    uint32_t dl_mbr_kbps{0};  // DL Maximum Bit Rate (kbps)
    uint32_t ul_gbr_kbps{0};  // UL Guaranteed Bit Rate (0 = non-GBR flow)
    uint32_t dl_gbr_kbps{0};  // DL Guaranteed Bit Rate (0 = non-GBR flow)
};

// =============================================================================
// RuleTable — Fast-path rule store with O(1) lookup indexes
//
// In production DPDK UPFs:
//   - PDR lookup uses rte_hash (cuckoo hash, ~80ns lookup including hash)
//   - FAR/QER use arrays indexed by rule ID (single cache line read)
//   - Rule updates from SMF (via N4 PFCP) must be lock-free or use RCU
//     so the fast-path lcore is never stalled by control-plane updates.
// =============================================================================
class RuleTable {
public:
    // ── Storage maps (rule_id → rule) ─────────────────────────────────────
    std::unordered_map<uint32_t, PDR> pdrs_;
    std::unordered_map<uint32_t, FAR> fars_;
    std::unordered_map<uint32_t, QER> qers_;

    // ── Classification indexes ─────────────────────────────────────────────
    // These are the "data plane tables" that the fast path queries per-packet.
    // Keyed by TEID (UL) and UE IP (DL) for O(1) lookup without scanning PDRs.
    std::unordered_map<uint32_t, uint32_t> teid_to_pdr_;   // TEID → PDR ID
    std::unordered_map<uint32_t, uint32_t> ueip_to_pdr_;   // UE IP → PDR ID

    void add_pdr(const PDR& pdr) {
        pdrs_[pdr.pdr_id] = pdr;
        if (pdr.dir == Direction::UL) {
            teid_to_pdr_[pdr.teid] = pdr.pdr_id;
        } else {
            ueip_to_pdr_[pdr.ue_ip] = pdr.pdr_id;
        }
    }

    void add_far(const FAR& far) {
        fars_[far.far_id] = far;
    }

    void add_qer(const QER& qer) {
        qers_[qer.qer_id] = qer;
    }

    // ── PDR lookup (per-packet, fast path) ───────────────────────────────
    // UL: key = TEID from GTP-U outer header
    // DL: key = destination IP of inner packet
    // Returns nullptr if no PDR matches (packet → pkts_no_pdr++, drop)
    const PDR* match_pdr(const Packet& pkt) const {
        uint32_t pdr_id = 0;
        if (pkt.dir == Direction::UL) {
            auto it = teid_to_pdr_.find(pkt.teid);
            if (it == teid_to_pdr_.end()) return nullptr;
            pdr_id = it->second;
        } else {
            auto it = ueip_to_pdr_.find(pkt.ue_ip);
            if (it == ueip_to_pdr_.end()) return nullptr;
            pdr_id = it->second;
        }
        auto it = pdrs_.find(pdr_id);
        return (it != pdrs_.end()) ? &it->second : nullptr;
    }

    const FAR* get_far(uint32_t far_id) const {
        auto it = fars_.find(far_id);
        return (it != fars_.end()) ? &it->second : nullptr;
    }

    const QER* get_qer(uint32_t qer_id) const {
        auto it = qers_.find(qer_id);
        return (it != qers_.end()) ? &it->second : nullptr;
    }
};

// =============================================================================
// build_demo_rules — Populate a RuleTable with num_ues simulated UE sessions
//
// Per UE we create:
//   UL PDR: matches TEID (1000 + ue_idx) from N3 gNB
//   DL PDR: matches UE IP (10.45.0.{1+ue_idx}) from N6
//   UL FAR: FORWARD to 8.8.8.8 (N6 internet gateway, no GTP-U encap)
//   DL FAR: FORWARD to gNB N3 IP (10.10.0.1) with GTP-U encap
//   QER:    QFI cycles: 1=voice, 2=video, 9=data — to model diverse traffic
//
// This mimics what the SMF sends in PFCP Session Establishment Requests
// for each UE PDU session.
// =============================================================================
inline RuleTable build_demo_rules(uint32_t num_ues) {
    RuleTable rt;

    // Base IP: 10.45.0.1 = 0x0A2D0001
    const uint32_t base_ue_ip = (10u << 24) | (45u << 16) | (0u << 8) | 1u;
    const uint32_t base_teid  = 1000u;

    // N6 internet gateway IP: 8.8.8.8
    const uint32_t n6_gw_ip  = (8u << 24) | (8u << 16) | (8u << 8) | 8u;
    // gNB N3 IP: 10.10.0.1
    const uint32_t gnb_n3_ip = (10u << 24) | (10u << 16) | (0u << 8) | 1u;

    // QFI cycle: voice, video, data
    const uint8_t qfi_cycle[]   = {1, 2, 9};
    // QoS params per QFI (MBR kbps, GBR kbps)
    const uint32_t mbr_ul[]     = {64,   4000, 100000};  // voice 64k, video 4M, data 100M
    const uint32_t mbr_dl[]     = {64,   8000, 100000};
    const uint32_t gbr_ul[]     = {64,   2000, 0};       // voice + video have GBR, data does not
    const uint32_t gbr_dl[]     = {64,   4000, 0};

    for (uint32_t i = 0; i < num_ues; ++i) {
        uint32_t ue_ip  = base_ue_ip + i;
        uint32_t teid   = base_teid  + i;
        uint8_t  qfi    = qfi_cycle[i % 3];
        uint32_t qi     = i % 3;

        // PDR IDs: UL = 2*i+1, DL = 2*i+2
        // FAR IDs: UL = 2*i+1, DL = 2*i+2
        // QER ID:  i+1
        uint32_t pdr_ul_id = 2 * i + 1;
        uint32_t pdr_dl_id = 2 * i + 2;
        uint32_t far_ul_id = 2 * i + 1;
        uint32_t far_dl_id = 2 * i + 2;
        uint32_t qer_id    = i + 1;

        // UL PDR
        PDR pdr_ul{};
        pdr_ul.pdr_id     = pdr_ul_id;
        pdr_ul.teid       = teid;
        pdr_ul.ue_ip      = ue_ip;
        pdr_ul.dir        = Direction::UL;
        pdr_ul.far_id     = far_ul_id;
        pdr_ul.qer_id     = qer_id;
        pdr_ul.precedence = 100;
        rt.add_pdr(pdr_ul);

        // DL PDR
        PDR pdr_dl{};
        pdr_dl.pdr_id     = pdr_dl_id;
        pdr_dl.teid       = teid;
        pdr_dl.ue_ip      = ue_ip;
        pdr_dl.dir        = Direction::DL;
        pdr_dl.far_id     = far_dl_id;
        pdr_dl.qer_id     = qer_id;
        pdr_dl.precedence = 100;
        rt.add_pdr(pdr_dl);

        // UL FAR: forward to N6 internet
        FAR far_ul{};
        far_ul.far_id     = far_ul_id;
        far_ul.action     = FarAction::FORWARD;
        far_ul.dst_ip     = n6_gw_ip;
        far_ul.dst_port   = 0;
        far_ul.encap_gtpu = false;  // UL: inner IP goes to internet, no re-encap
        rt.add_far(far_ul);

        // DL FAR: forward to gNB with GTP-U encapsulation
        FAR far_dl{};
        far_dl.far_id     = far_dl_id;
        far_dl.action     = FarAction::FORWARD;
        far_dl.dst_ip     = gnb_n3_ip;
        far_dl.dst_port   = 2152;   // GTP-U port (IANA)
        far_dl.encap_gtpu = true;
        rt.add_far(far_dl);

        // QER
        QER qer{};
        qer.qer_id      = qer_id;
        qer.qfi         = qfi;
        qer.ul_mbr_kbps = mbr_ul[qi];
        qer.dl_mbr_kbps = mbr_dl[qi];
        qer.ul_gbr_kbps = gbr_ul[qi];
        qer.dl_gbr_kbps = gbr_dl[qi];
        rt.add_qer(qer);
    }

    return rt;
}

} // namespace upf
