#pragma once
#include <atomic>
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "ims/sip.h"

// ============================================================
// IMS HSS — Home Subscriber Server (IMS extension)
//
// SAME HSS as 4G EPC but with additional interfaces:
//   S6a: MME ↔ HSS (already implemented in Phase 2)
//   Cx:  S-CSCF/I-CSCF ↔ HSS (this file — TS 29.229)
//   Sh:  AS (MTAS) ↔ HSS (subscriber data for application services)
//
// Cx PROCEDURES:
//   UAR/UAA: I-CSCF asks "which S-CSCF should serve user X?"
//   SAR/SAA: S-CSCF registers itself for user X, gets subscriber profile
//   LIR/LIA: I-CSCF asks "where is user X currently registered?"
//   MAR/MAA: S-CSCF fetches auth vectors for IMS-AKA
//
// IMS-AKA vs EPS-AKA:
//   Both use Milenage algorithm with Ki
//   IMS-AKA: SIP-level auth (in REGISTER message)
//   EPS-AKA: NAS-level auth (in Attach Request — our Phase 2)
//   UE does BOTH: EPS-AKA for 4G data, IMS-AKA for IMS registration
//
// Our sim: simplified Cx (SAR/SAA only)
// Port: TCP 3870
// ============================================================
class ImsHssNode {
public:
    ImsHssNode(std::atomic<bool>& stop, std::atomic<bool>& ims_hss_ready);
    void run();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& ims_hss_ready_;

    Socket server_socket_;
    Socket scscf_conn_;
    uint32_t next_seq_{1};

    void setupServer();
    void receiveLoop();
    void handleSAR(const std::vector<uint8_t>& payload);
};
