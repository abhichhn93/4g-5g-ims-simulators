#pragma once
// ============================================================
// NAS-EPS (TS 24.301) message builders — produce the exact byte
// sequences embedded as NAS-PDU inside S1AP messages, so
// Wireshark's native NAS-EPS dissector (layered under S1AP)
// shows real message names (Attach request, Authentication
// request, Security mode command, ...).
//
// All byte layouts here were derived + verified against
// Wireshark/tshark via pycrate (see project memory
// s1ap_aper_plan.md for the derivation).
// ============================================================
#include <cstdint>
#include <vector>

namespace nas_eps {

// Encode a 15-digit IMSI as an 8-byte EPS Mobile Identity (TS 24.008 §10.5.1.4,
// type=001=IMSI). Shared with s1ap_codec for the InitialUEMessage EPS ID IE.
std::vector<uint8_t> encodeEpsIdImsi(uint64_t imsi);

// EMM Attach Request + ESM PDN Connectivity Request (21 bytes).
// imsi must be a 15-digit IMSI (e.g. 404100000000001).
std::vector<uint8_t> buildAttachRequest(uint64_t imsi);

// EMM Authentication Request (36 bytes): RAND[16] + AUTN[16].
std::vector<uint8_t> buildAuthRequest(const uint8_t rand16[16], const uint8_t autn16[16]);

// EMM Authentication Response (11 bytes): RES[8].
std::vector<uint8_t> buildAuthResponse(const uint8_t res8[8]);

// EMM Attach Accept + ESM Activate Default EPS Bearer Context Request (34 bytes).
// ue_ip is the UE's PDN IPv4 address (allocated by P-GW).
std::vector<uint8_t> buildAttachAccept(const uint8_t ue_ip[4]);

// Fixed-content NAS messages (no per-UE variation).
extern const std::vector<uint8_t> SECURITY_MODE_COMMAND;   // 7 bytes
extern const std::vector<uint8_t> SECURITY_MODE_COMPLETE;  // 2 bytes
extern const std::vector<uint8_t> ATTACH_COMPLETE;         // 7 bytes (incl. ESM Activate Default Bearer Accept)

// TAU (Tracking Area Update) NAS messages — TS 24.301 §5.5.3
// TAU Request: UE → eNB → MME when UE enters a new Tracking Area
std::vector<uint8_t> buildTauRequest(uint64_t imsi);
// TAU Accept: MME → eNB → UE confirming the new TAI, updating T3412 timer
std::vector<uint8_t> buildTauAccept();

} // namespace nas_eps
