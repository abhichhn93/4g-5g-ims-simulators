#pragma once
#include <atomic>
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "ims/sip.h"

// ============================================================
// P-CSCF — Proxy-Call Session Control Function
//
// REAL ROLE (TS 23.228):
//   - FIRST point of contact for IMS UE
//   - UE discovers P-CSCF via PCO (Protocol Configuration Options)
//     in PDN Connectivity Response during 4G attach
//   - Forwards SIP REGISTER to I-CSCF (using DNS to find home network)
//   - Forwards SIP INVITE toward S-CSCF
//   - Maintains IPSec tunnel with UE for SIP signalling security
//   - Interfaces with PCRF via Rx (Diameter) to:
//     → Request QCI=1 dedicated bearer when call established
//     → Release bearer when call ends (BYE)
//   - In visited network (roaming): P-CSCF is in visited NW,
//     S-CSCF is in home NW. P-CSCF connects to I-CSCF in home NW.
//
// ERICSSON MTAS CONTEXT:
//   P-CSCF is NOT where MTAS lives.
//   MTAS is invoked by S-CSCF as a 3rd-party AS.
//   P-CSCF just proxies — it has no service logic.
//
// OUR SIM:
//   TCP server on port 5060 (real SIP port)
//   Receives from UE (simulated by main/eNB)
//   Forwards to S-CSCF on port 5070
//   Sends Diameter Rx to PCRF when call media confirmed
//
// PORTS:
//   5060 = SIP (standard) — P-CSCF listens here
//   5061 = SIP TLS (real networks use this, we skip)
// ============================================================
class PcscfNode {
public:
    PcscfNode(std::atomic<bool>& stop, std::atomic<bool>& pcscf_ready);
    void run();

    // Called from IMS main to inject a UE action
    void submitUeAction(const std::string& action, const std::string& impu,
                        const std::string& call_id = "");

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& pcscf_ready_;

    Socket server_socket_;
    Socket ue_conn_;      // connection from UE (simulated)
    Socket scscf_conn_;   // connection to S-CSCF

    uint32_t next_seq_{1};

    void setupServer();
    void receiveLoop();
    void handleRegister  (const std::vector<uint8_t>& payload);
    void handleInvite    (const std::vector<uint8_t>& payload);
    void handle200Ok     (const std::vector<uint8_t>& payload);
    void handle180Ringing(const std::vector<uint8_t>& payload);
    void handleBye       (const std::vector<uint8_t>& payload);

    void sendRxAAR(const std::string& impu, const std::string& call_id);
    void forwardToSCscf(const std::vector<uint8_t>& frame);
    void forwardToUe   (const std::vector<uint8_t>& frame);
};
