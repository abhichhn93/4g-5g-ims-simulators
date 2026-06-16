#pragma once
#include <cstdint>

// ============================================================
// MESSAGE TYPES — every frame on the wire starts with this
//
// Phase 1: S1AP_INITIAL_UE_MSG
// Phase 2: + DL/UL NAS Transport + Diameter AIR/AIA
// Phase 3: + GTP-C Create/Modify Session + S1AP ICSR/ICSRSP
// Phase 4: + Delete Session, Detach, Dedicated Bearer (PCRF Gx)
// Phase 5: + TAU (Tracking Area Update), S1 Handover
// ============================================================
enum class MessageType : uint16_t {
    // ── S1AP (eNB ↔ MME, TCP 38412) ─────────────────────────
    S1AP_INITIAL_UE_MSG            = 0x0001,  // eNB→MME: UE connects, Attach Request
    S1AP_DL_NAS_TRANSPORT          = 0x0002,  // MME→eNB: forward NAS to UE
    S1AP_UL_NAS_TRANSPORT          = 0x0003,  // eNB→MME: forward NAS from UE
    S1AP_INITIAL_CONTEXT_SETUP_REQ = 0x0004,  // MME→eNB: setup bearers, deliver Attach Accept
    S1AP_INITIAL_CONTEXT_SETUP_RSP = 0x0005,  // eNB→MME: bearers ready, eNB's S1-U TEID

    // ── TAU (Tracking Area Update, TS 24.301 §5.5.3) ────────
    S1AP_TAU_REQUEST               = 0x0006,  // eNB→MME: NAS TAU Request (UL NAS Transport)
    S1AP_TAU_ACCEPT                = 0x0007,  // MME→eNB: NAS TAU Accept  (DL NAS Transport)

    // ── S1 Handover (TS 36.413 §8.4) ────────────────────────
    S1AP_HANDOVER_REQUIRED         = 0x0010,  // eNB→MME: src eNB triggers HO
    S1AP_HANDOVER_REQUEST          = 0x0011,  // MME→tgt eNB: prepare resources
    S1AP_HANDOVER_REQUEST_ACK      = 0x0012,  // tgt eNB→MME: resources ready, transparent container
    S1AP_HANDOVER_COMMAND          = 0x0013,  // MME→src eNB: tell UE to move (includes target container)
    S1AP_ENB_STATUS_TRANSFER       = 0x0014,  // src eNB→MME: PDCP status for lossless HO
    S1AP_MME_STATUS_TRANSFER       = 0x0015,  // MME→tgt eNB: forward PDCP status
    S1AP_HANDOVER_NOTIFY           = 0x0016,  // tgt eNB→MME: UE arrived on target
    S1AP_UE_CONTEXT_RELEASE_CMD    = 0x0017,  // MME→src eNB: release old resources
    S1AP_UE_CONTEXT_RELEASE_CMPL   = 0x0018,  // src eNB→MME: resources released

    // ── Diameter S6a (MME ↔ HSS, TCP 3868) ──────────────────
    DIA_AIR = 0x0101,  // Auth-Information-Request: MME→HSS
    DIA_AIA = 0x0102,  // Auth-Information-Answer: HSS→MME

    // ── GTP-C S11 (MME ↔ S-GW, UDP 2123) ───────────────────
    GTP_CREATE_SESSION_REQ = 0x0201,  // MME→S-GW (S11), S-GW→P-GW (S5)
    GTP_CREATE_SESSION_RSP = 0x0202,  // P-GW→S-GW (S5), S-GW→MME (S11)
    GTP_MODIFY_BEARER_REQ  = 0x0203,  // MME→S-GW: S1-U TEID update (TAU / HO path switch)
    GTP_MODIFY_BEARER_RSP  = 0x0204,  // S-GW→MME: acknowledged

    // ── Diameter Gx (P-GW ↔ PCRF, TCP 3869) ─────────────────
    DIA_GX_CCR_I = 0x0401,  // Credit-Control-Request Initial: P-GW→PCRF
    DIA_GX_CCA_I = 0x0402,  // Credit-Control-Answer Initial:  PCRF→P-GW
};

inline const char* msg_type_str(MessageType t) {
    switch (t) {
        case MessageType::S1AP_INITIAL_UE_MSG:            return "S1AP_InitialUEMsg";
        case MessageType::S1AP_DL_NAS_TRANSPORT:          return "S1AP_DL_NAS_Transport";
        case MessageType::S1AP_UL_NAS_TRANSPORT:          return "S1AP_UL_NAS_Transport";
        case MessageType::S1AP_INITIAL_CONTEXT_SETUP_REQ: return "S1AP_InitialContextSetupReq";
        case MessageType::S1AP_INITIAL_CONTEXT_SETUP_RSP: return "S1AP_InitialContextSetupRsp";
        case MessageType::S1AP_TAU_REQUEST:               return "S1AP_TAU_Request";
        case MessageType::S1AP_TAU_ACCEPT:                return "S1AP_TAU_Accept";
        case MessageType::S1AP_HANDOVER_REQUIRED:         return "S1AP_HandoverRequired";
        case MessageType::S1AP_HANDOVER_REQUEST:          return "S1AP_HandoverRequest";
        case MessageType::S1AP_HANDOVER_REQUEST_ACK:      return "S1AP_HandoverRequestAck";
        case MessageType::S1AP_HANDOVER_COMMAND:          return "S1AP_HandoverCommand";
        case MessageType::S1AP_ENB_STATUS_TRANSFER:       return "S1AP_eNBStatusTransfer";
        case MessageType::S1AP_MME_STATUS_TRANSFER:       return "S1AP_MMEStatusTransfer";
        case MessageType::S1AP_HANDOVER_NOTIFY:           return "S1AP_HandoverNotify";
        case MessageType::S1AP_UE_CONTEXT_RELEASE_CMD:    return "S1AP_UEContextReleaseCmd";
        case MessageType::S1AP_UE_CONTEXT_RELEASE_CMPL:   return "S1AP_UEContextReleaseCmpl";
        case MessageType::DIA_AIR:                        return "Diameter_AIR";
        case MessageType::DIA_AIA:                        return "Diameter_AIA";
        case MessageType::GTP_CREATE_SESSION_REQ:         return "GTP_CreateSessionReq";
        case MessageType::GTP_CREATE_SESSION_RSP:         return "GTP_CreateSessionRsp";
        case MessageType::GTP_MODIFY_BEARER_REQ:          return "GTP_ModifyBearerReq";
        case MessageType::GTP_MODIFY_BEARER_RSP:          return "GTP_ModifyBearerRsp";
        case MessageType::DIA_GX_CCR_I:                  return "Diameter_Gx_CCR-I";
        case MessageType::DIA_GX_CCA_I:                  return "Diameter_Gx_CCA-I";
        default:                                          return "UNKNOWN";
    }
}
