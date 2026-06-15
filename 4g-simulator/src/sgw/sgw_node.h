#pragma once
#include <atomic>
#include <unordered_map>
#include <mutex>
#include "common/socket_wrapper.h"
#include "common/tlv.h"

// ============================================================
// S-GW NODE — Serving Gateway
//
// REAL S-GW (TS 23.401 §4.4.3.3):
//   Control plane (S11/S5/S8): GTP-C over SCTP/UDP
//   User plane   (S1-U/S5-U):  GTP-U over UDP
//   Interfaces:
//     S11: MME  ↔ S-GW  (control, GTP-C port 2123)
//     S5:  S-GW ↔ P-GW  (control, same port in flat network)
//     S1-U: eNB ↔ S-GW  (data, GTP-U port 2152)
//     S5-U: S-GW ↔ P-GW (data, GTP-U port 2152)
//   Roles:
//     - Allocates and manages GTP TEIDs for both control and data planes
//     - Routes downlink data to correct eNB based on eNB's S1-U TEID
//     - Local mobility anchor (UE can change eNB without changing S-GW)
//     - Buffers downlink data when UE is in idle mode (ECM-IDLE)
//     - Generates Charging Data Records (CDRs) for offline charging
//
// OUR SIMULATOR (Phase 3):
//   - Control plane only (no GTP-U user plane)
//   - UDP server on port 2123 (S11 interface, from MME)
//   - UDP client to P-GW on port 2124 (S5 interface)
//   - Allocates TEIDs using atomic counter
//   - Stores bearer context per TEID
//
// THREADING:
//   - sgw_th runs sgw_node.run()
//   - Single receive loop on UDP port 2123
//   - Synchronous: receives from MME → forwards to P-GW → gets response → replies to MME
//
// INTERVIEW Q: "What is the S-GW's role during attach?"
// ANSWER: "S-GW is the local mobility anchor. It allocates control-plane TEIDs
//   (S11 = S-GW↔MME, S5 = S-GW↔P-GW) and data-plane TEIDs (S1-U = S-GW↔eNB).
//   It forwards Create Session Request to P-GW to get the UE IP address.
//   After attach, all downlink user-plane packets come from P-GW via GTP-U
//   and S-GW routes them to the correct eNB based on eNB's S1-U TEID."
// ============================================================
class SgwNode {
public:
    SgwNode(std::atomic<bool>& stop, std::atomic<bool>& sgw_ready);
    void run();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& sgw_ready_;

    UdpSocket s11_socket_;  // S11: MME ↔ S-GW (UDP 2123)
    UdpSocket s5_socket_;   // S5:  S-GW ↔ P-GW (UDP port assigned by OS)

    std::atomic<uint32_t> next_teid_{1000};  // TEID counter (S-GW's TEIDs start at 1000)
    uint32_t next_seq_{1};

    void setupSockets();
    void receiveLoop();
    void handleCreateSessionReq(const std::vector<uint8_t>& payload, const sockaddr_in& mme_addr);
    void handleModifyBearerReq (const std::vector<uint8_t>& payload, const sockaddr_in& mme_addr);
};
