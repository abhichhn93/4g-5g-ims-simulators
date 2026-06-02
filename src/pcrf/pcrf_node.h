#pragma once
#include <atomic>
#include "common/socket_wrapper.h"
#include "common/tlv.h"

// ============================================================
// PCRF NODE — Policy and Charging Rules Function
//
// REAL PCRF (TS 23.203, TS 29.212):
//   - Stores and enforces subscriber policy (per-APN, per-service)
//   - Gx interface: P-GW ↔ PCRF (Diameter)
//   - Gy interface: P-GW ↔ OCS (Online Charging System) — real-time balance
//   - Gz interface: P-GW ↔ OFCS (Offline Charging) — CDR generation
//   - Rx interface: P-CSCF ↔ PCRF — IMS-triggered bearer rules (Phase 5)
//
// DIAMETER Gx FLOW (TS 29.212 §4.5.1):
//   P-GW → PCRF: CCR-I (Credit-Control-Request Initial)
//     - User-Name (IMSI), Called-Station-Id (APN)
//     - QoS-Information (requested QCI, bitrate)
//     - Framed-IP-Address (UE's IP)
//   PCRF → P-GW: CCA-I (Credit-Control-Answer Initial)
//     - Charging-Rule-Install: rules to apply (name, QCI, rating group)
//     - QoS-Information: approved bitrate (may be lower than requested)
//     - Online/Offline charging: enabled/disabled
//
// OUR SIMULATOR (Phase 4):
//   - TCP server on port 3869
//   - Accepts connection from P-GW
//   - Receives CCR-I, logs subscriber identity and APN
//   - Returns CCA-I with a "permit_internet" charging rule
//   - Phase 5: add Rx interface, trigger dedicated bearer via RAR message
//
// 5G EQUIVALENT: PCF (Policy Control Function)
//   - Replaces PCRF + PCEF role split
//   - Uses N7 reference point (PCF ↔ SMF) via HTTP/2 + JSON
//   - Policy decisions via "Policy Authorization" (was Rx) + AM Policy
//   - INTERVIEW: "5G PCF does what PCRF did but over REST/HTTP, not Diameter.
//     This lets PCF scale horizontally as stateless cloud-native pods."
//
// THREADING:
//   - pcrf_th → PcrfNode::run()
//   - Single receive loop (synchronous CCR/CCA per P-GW request)
// ============================================================
class PcrfNode {
public:
    PcrfNode(std::atomic<bool>& stop, std::atomic<bool>& pcrf_ready);
    void run();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& pcrf_ready_;

    Socket server_socket_;
    Socket pgw_conn_;

    uint32_t next_seq_{1};

    void setupServer();
    void receiveLoop();
    void handleCCR(const std::vector<uint8_t>& payload);
};
