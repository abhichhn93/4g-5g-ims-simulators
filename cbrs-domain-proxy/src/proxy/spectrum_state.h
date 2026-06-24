#pragma once
#include <string>

// ============================================================
// SPECTRUM ALLOCATION STATE MACHINE
//
// Each CBSD managed by the Domain Proxy goes through this lifecycle.
// These states map directly to the WInnForum CBRS specification
// (WINNF-TS-0016, Section 6).
//
// Transitions:
//
//   UNREGISTERED
//       │  RegistrationRequest (CBSD sends FCC ID, location, antenna info)
//       ▼
//   REGISTERED
//       │  GrantRequest (CBSD asks for a frequency + bandwidth allocation)
//       ▼
//   GRANTED          ← SAS assigned a grant ID, not yet transmitting
//       │  HeartbeatRequest (CBSD confirms it is ready to transmit)
//       ▼
//   AUTHORIZED       ← CBSD is actively transmitting on the granted channel
//       │  RelinquishmentRequest (CBSD voluntarily gives up the grant)
//       │  OR grant expires (SAS-controlled heartbeat interval timeout)
//       ▼
//   REGISTERED       ← back to registered, can request a new grant
//       │  DeregistrationRequest
//       ▼
//   UNREGISTERED
//
// The Domain Proxy enforces these transitions — it rejects out-of-order
// messages (e.g., a GrantRequest before registration) before forwarding
// anything to the SAS.
// ============================================================

enum class SpectrumState {
    UNREGISTERED,  // CBSD connected but not yet registered with SAS
    REGISTERED,    // Registration accepted; no active spectrum grant
    GRANTED,       // Grant assigned by SAS; CBSD acknowledged, not yet TX
    AUTHORIZED     // CBSD is transmitting on the granted channel
};

inline std::string stateToString(SpectrumState s) {
    switch (s) {
        case SpectrumState::UNREGISTERED: return "UNREGISTERED";
        case SpectrumState::REGISTERED:   return "REGISTERED";
        case SpectrumState::GRANTED:      return "GRANTED";
        case SpectrumState::AUTHORIZED:   return "AUTHORIZED";
    }
    return "UNKNOWN";
}

// SAS response codes (WINNF-TS-0016 Table 14)
// 0 = SUCCESS, 100-series = unsupported/invalid, 200-series = reg errors,
// 300-series = grant errors, 400-series = heartbeat errors
namespace ResponseCode {
    constexpr int SUCCESS                 = 0;
    constexpr int VERSION_MISMATCH        = 100;
    constexpr int BLACKLISTED             = 101;
    constexpr int MISSING_PARAM           = 102;
    constexpr int INVALID_VALUE           = 103;
    constexpr int CERTIFICATE_ERROR       = 104;
    constexpr int DEREGISTER              = 105;   // SAS-initiated deregister
    constexpr int REG_PENDING             = 200;
    constexpr int GROUP_ERROR             = 201;
    constexpr int UNSUPPORTED_SPECTRUM    = 300;
    constexpr int INTERFERENCE            = 301;
    constexpr int GRANT_CONFLICT          = 302;
    constexpr int TERMINATED_GRANT        = 400;
    constexpr int SUSPENDED_GRANT         = 401;
    constexpr int UNSYNC_OP_PARAM         = 402;
}
