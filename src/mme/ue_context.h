#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class EmmState : uint8_t {
    DEREGISTERED          = 0,
    REGISTERED_INITIATED  = 1,   // Attach Request received
    AUTH_PENDING          = 2,   // Auth Request sent, waiting for UE RES
    SESSION_PENDING       = 3,   // GTP-C Create Session in progress
    REGISTERED            = 4,   // Fully attached, bearer established, UE has IP
    DEREGISTERED_INITIATED = 5,  // Detach in progress (Phase 4)
};

inline std::string emm_state_str(EmmState s) {
    switch(s) {
        case EmmState::DEREGISTERED:           return "DEREGISTERED";
        case EmmState::REGISTERED_INITIATED:   return "REGISTERED_INITIATED";
        case EmmState::AUTH_PENDING:           return "AUTH_PENDING";
        case EmmState::SESSION_PENDING:        return "SESSION_PENDING";
        case EmmState::REGISTERED:             return "REGISTERED";
        case EmmState::DEREGISTERED_INITIATED: return "DEREGISTERED_INITIATED";
        default:                               return "UNKNOWN";
    }
}

// ============================================================
// AUTH VECTORS — from HSS via Diameter AIA
//
// REAL AKA (TS 33.102): Uses Milenage algorithm (AES-based) with Ki.
// OUR SIM: RAND=random, AUTN=RAND^0xAA, XRES=RAND^0x55, Kasme=zeros
//
// INTERVIEW: "AKA = mutual authentication. UE verifies AUTN (proves network
//   knows Ki) before responding with RES. 4G vulnerability: IMSI in cleartext
//   on first attach. 5G fix: SUCI (encrypted IMSI)."
// ============================================================
struct AuthVectors {
    uint8_t rand[16];   // Random challenge (16 bytes)
    uint8_t autn[16];   // Auth Token — UE verifies network with this
    uint8_t xres[8];    // Expected Response — MME compares UE's RES with this
    uint8_t kasme[32];  // Base key — NAS/RRC keys derived from this (Phase 3+)
};

// ============================================================
// EPS BEARER CONTEXT — established during GTP-C session creation
//
// REAL BEARER:
//   - Default bearer created on attach (EBI=5, QCI=9 internet)
//   - Dedicated bearers added for QoS-sensitive traffic (EBI=6+, QCI=1 VoLTE)
//   - Data path: UE ←RLC/MAC→ eNB ←GTP-U/S1-U→ S-GW ←GTP-U/S5-U→ P-GW ←→ Internet
//   - Control path: MME ←GTP-C/S11→ S-GW ←GTP-C/S5→ P-GW ←Gx→ PCRF
//
// TEID (Tunnel Endpoint Identifier):
//   - 32-bit number that labels a GTP tunnel
//   - Routing: S-GW receives a GTP-U packet, reads TEID, routes to correct UE
//   - Assigned by the RECEIVER: eNB assigns its S1-U TEID, S-GW assigns its S1-U TEID
//   - Both sides exchange TEIDs during Create Session / Modify Bearer
//
// INTERVIEW Q: "What is a TEID and why do we need it?"
// ANSWER: "TEID is like a port number for GTP tunnels. It lets the S-GW
//   demultiplex packets from many UEs arriving on one UDP socket.
//   Receiver allocates the TEID and tells the sender — same concept
//   as TCP ports, but for the GTP user-plane tunnel."
// ============================================================
struct Bearer {
    uint8_t  ebi;              // EPS Bearer ID (5=default, 6-15=dedicated)
    uint8_t  qci;              // QoS Class Indicator (9=internet, 1=VoLTE, 4=gaming)
    uint32_t sgw_s11_teid;    // S-GW's GTP-C TEID on S11 (control plane)
    uint32_t sgw_s1u_teid;    // S-GW's GTP-U TEID on S1-U (user plane, downlink)
    uint32_t pgw_s5_teid;     // P-GW's GTP-C TEID on S5 (control plane)
    uint32_t enb_s1u_teid;    // eNB's GTP-U TEID on S1-U (user plane, uplink) — set in Modify Bearer
    std::string ue_ip;        // UE's IPv4 address (allocated by P-GW)
};

// ============================================================
// UE CONTEXT — what MME stores per attached UE
//
// LIFECYCLE:
//   Created: on InitialUEMessage (Attach Request arrives)
//   Updated: auth vectors, bearer TEIDs, EMM state transitions
//   Deleted: on Detach Complete or implicit detach (T3412 expiry)
//
// MEMORY: In Phase 3 this is stored in shared_ptr<UeContext> inside
//   the sharded UeContextStore. Multiple threads may hold the shared_ptr.
//   The UE context is freed when ALL shared_ptr references drop.
//
// INTERVIEW Q: "Why shared_ptr for UE contexts?"
// ANSWER: "Multiple threads hold references — the eNB receive thread,
//   the GTP-C handler, and the STATUS command thread. shared_ptr ensures
//   the context lives as long as anyone needs it. The context is only
//   freed when the last holder drops its reference (after detach + cleanup)."
// ============================================================
struct UeContext {
    // Identity
    uint64_t imsi;
    uint32_t mme_ue_s1ap_id;
    uint32_t enb_ue_s1ap_id;

    // State
    EmmState emm_state{EmmState::DEREGISTERED};

    // Phase 2: Authentication
    AuthVectors auth;

    // Phase 3: Bearer (data plane)
    std::vector<Bearer> bearers;

    // Location
    uint16_t tai_mcc{0}, tai_mnc{0}, tai_tac{0};

    // Phase 4: latency tracking (set when InitialUEMsg received)
    std::chrono::steady_clock::time_point attach_start;

    // Phase 4: Flyweight — shared_ptr to immutable subscriber profile
    // All "internet" UEs point to the SAME SubscriberProfile object
    // Interview: "This is the Flyweight pattern — separate shared intrinsic
    //   state from per-UE extrinsic state. Saves memory at scale."
    std::shared_ptr<const void> profile;  // type-erased to avoid circular include

    // Helpers
    Bearer* defaultBearer() {
        for(auto& b : bearers) if(b.ebi==5) return &b;
        return nullptr;
    }
    const Bearer* defaultBearer() const {
        for(const auto& b : bearers) if(b.ebi==5) return &b;
        return nullptr;
    }
};
