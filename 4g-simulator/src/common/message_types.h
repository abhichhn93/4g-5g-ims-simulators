#pragma once
#include <cstdint>

// ============================================================
// MESSAGE TYPES — every frame on the wire starts with this
//
// Phase 1: S1AP_INITIAL_UE_MSG
// Phase 2: + DL/UL NAS Transport + Diameter AIR/AIA
// Phase 3: + GTP-C Create/Modify Session + S1AP ICSR/ICSRSP
// Phase 4: + Delete Session, Detach, Dedicated Bearer (PCRF Gx)
// ============================================================
enum class MessageType : uint16_t {
    // ── S1AP (eNB ↔ MME, TCP 38412) ─────────────────────────
    S1AP_INITIAL_UE_MSG         = 0x0001,  // eNB→MME: UE connects, Attach Request
    S1AP_DL_NAS_TRANSPORT       = 0x0002,  // MME→eNB: forward NAS to UE
    S1AP_UL_NAS_TRANSPORT       = 0x0003,  // eNB→MME: forward NAS from UE
    S1AP_INITIAL_CONTEXT_SETUP_REQ = 0x0004, // MME→eNB: setup bearers, deliver Attach Accept
    S1AP_INITIAL_CONTEXT_SETUP_RSP = 0x0005, // eNB→MME: bearers ready, eNB's S1-U TEID

    // ── Diameter S6a (MME ↔ HSS, TCP 3868) ──────────────────
    DIA_AIR = 0x0101,  // Auth-Information-Request: MME→HSS
    DIA_AIA = 0x0102,  // Auth-Information-Answer: HSS→MME

    // ── GTP-C S11 (MME ↔ S-GW, UDP 2123) ───────────────────
    // TS 29.274 — GTPv2-C message types
    GTP_CREATE_SESSION_REQ = 0x0201,  // MME→S-GW (S11), S-GW→P-GW (S5)
    GTP_CREATE_SESSION_RSP = 0x0202,  // P-GW→S-GW (S5), S-GW→MME (S11)
    GTP_MODIFY_BEARER_REQ  = 0x0203,  // MME→S-GW: tell S-GW the eNB's S1-U TEID
    GTP_MODIFY_BEARER_RSP  = 0x0204,  // S-GW→MME: acknowledged

    // ── Diameter Gx (P-GW ↔ PCRF, TCP 3869) ─────────────────
    // TS 29.212 — Policy and Charging Control
    DIA_GX_CCR_I = 0x0401,  // Credit-Control-Request Initial: P-GW→PCRF
    DIA_GX_CCA_I = 0x0402,  // Credit-Control-Answer Initial:  PCRF→P-GW

    // Phase 4+:
    // GTP_DELETE_SESSION_REQ = 0x0205,
    // GTP_DELETE_SESSION_RSP = 0x0206,
    // DIA_CCR_I = 0x0301,  // Gx: Credit-Control-Request Initial (P-GW→PCRF)
    // DIA_CCA_I = 0x0302,  // Gx: Credit-Control-Answer Initial (PCRF→P-GW)
};

inline const char* msg_type_str(MessageType t) {
    switch (t) {
        case MessageType::S1AP_INITIAL_UE_MSG:          return "S1AP_InitialUEMsg";
        case MessageType::S1AP_DL_NAS_TRANSPORT:        return "S1AP_DL_NAS_Transport";
        case MessageType::S1AP_UL_NAS_TRANSPORT:        return "S1AP_UL_NAS_Transport";
        case MessageType::S1AP_INITIAL_CONTEXT_SETUP_REQ: return "S1AP_InitialContextSetupReq";
        case MessageType::S1AP_INITIAL_CONTEXT_SETUP_RSP: return "S1AP_InitialContextSetupRsp";
        case MessageType::DIA_AIR:                      return "Diameter_AIR";
        case MessageType::DIA_AIA:                      return "Diameter_AIA";
        case MessageType::GTP_CREATE_SESSION_REQ:       return "GTP_CreateSessionReq";
        case MessageType::GTP_CREATE_SESSION_RSP:       return "GTP_CreateSessionRsp";
        case MessageType::GTP_MODIFY_BEARER_REQ:        return "GTP_ModifyBearerReq";
        case MessageType::GTP_MODIFY_BEARER_RSP:        return "GTP_ModifyBearerRsp";
        case MessageType::DIA_GX_CCR_I:                return "Diameter_Gx_CCR-I";
        case MessageType::DIA_GX_CCA_I:                return "Diameter_Gx_CCA-I";
        default:                                        return "UNKNOWN";
    }
}
