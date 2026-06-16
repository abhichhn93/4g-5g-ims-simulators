#pragma once
// ============================================================
// P-CSCF — Proxy-Call Session Control Function (Multi-UE)
//
// REAL ROLE: First SIP contact for UE. Routes to S-CSCF.
// Manages Rx interface to PCRF for QCI=1 bearer.
//
// THIS VERSION: accepts connections from ue_sim A, B, C
// simultaneously. Routes INVITE to correct UE by IMPU.
//
// ROUTING LOGIC:
//   UE→ P-CSCF: extract IMPU from From/To, register socket
//   P-CSCF→ S-CSCF: forward all UE requests
//   S-CSCF→ P-CSCF: if INVITE, route to callee UE socket
//                    if response, route to caller UE socket
//
// THREADING:
//   accept_th: accepts new UE TCP connections
//   per_ue_th[N]: one receive thread per connected UE
//   scscf_rx_th:  receives from S-CSCF, routes to UEs
// ============================================================
#include <atomic>
#include <mutex>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "ims/sip.h"

struct UeSession {
    std::string impu;       // registered IMPU
    std::string ip;         // UE's 4G IP
    Socket      sock;       // TCP socket to this UE
    bool        registered{false};
};

class PcscfNode {
public:
    PcscfNode(std::atomic<bool>& stop, std::atomic<bool>& pcscf_ready);
    void run();
    void printStatus();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& pcscf_ready_;

    Socket              server_socket_;
    Socket              scscf_conn_;

    // UE registry — IMPU → session (protected by mtx_)
    std::mutex                          ue_mtx_;
    std::map<std::string, UeSession*>   ue_by_impu_;  // IMPU → session ptr
    std::vector<std::shared_ptr<UeSession>> ue_sessions_;
    std::vector<std::thread>            ue_threads_;

    // Call routing — Call-ID → caller IMPU
    std::mutex                          call_mtx_;
    std::map<std::string, std::string>  call_to_caller_;      // call_id → caller impu
    std::map<std::string, std::string>  call_to_callee_;      // call_id → callee impu
    std::set<std::string>               call_invite_delivered_; // call_ids whose INVITE was forwarded to callee

    std::atomic<uint32_t> next_seq_{1};

    void connectToSCscf();
    void acceptLoop();
    void ueReceiveLoop(UeSession* ses);
    void scscfReceiveLoop();

    // From UE → forward to S-CSCF
    void handleFromUe(UeSession* ses, const std::vector<uint8_t>& payload);

    // From S-CSCF → route to correct UE
    void handleFromScscf(const std::vector<uint8_t>& payload);

    void sendToScscf(const std::vector<uint8_t>& frame);
    void sendToUe(const std::string& impu, const std::vector<uint8_t>& frame);
    void sendRxAAR(const std::string& impu, const std::string& call_id);
};
