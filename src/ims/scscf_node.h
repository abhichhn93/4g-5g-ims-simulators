#pragma once
#include <atomic>
#include <map>
#include <string>
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "ims/sip.h"

struct ImsSubscriber {
    std::string impu;       // Public identity: sip:+919...@ims.domain
    std::string impi;       // Private identity: 9...@ims.domain
    std::string contact;    // Current contact: sip:ue@10.0.0.1:5060
    std::string scscf_name; // Which S-CSCF this user is assigned to
    bool        registered{false};
};

// ============================================================
// S-CSCF — Serving-Call Session Control Function
//        + MTAS (Media Telephony Application Server) hooks
//
// REAL ROLE (TS 23.228):
//   - SIP Registrar: stores UE's contact binding
//   - SIP Proxy: routes calls based on filter criteria
//   - Invokes Application Servers (like MTAS) via ISC interface
//   - Authenticates UE using IMS-AKA (via HSS Cx interface)
//   - Handles service logic: call forwarding, barring, waiting
//
// MTAS (Ericsson specific):
//   - Application Server triggered by S-CSCF via 3rd party SIP
//   - Handles: VoLTE codec negotiation, supplementary services
//     OIP/OIR (Originating Identity Presentation)
//     TIP/TIS (Terminating Identity Presentation)
//     CONF (Multi-Party Conference)
//     MMTEL (Multimedia Telephony) service features
//   - MTAS receives REGISTER copy → applies service profile
//   - MTAS receives INVITE → may modify SDP, apply policies
//
// ISC (IMS Service Control) interface: S-CSCF ↔ MTAS
//   Standard SIP interface (RFC 3261)
//   Trigger Points (TP) in service profile determine when MTAS is invoked
//
// Cx interface: S-CSCF ↔ HSS (Diameter)
//   SAR (Server-Assignment-Request): S-CSCF registers, gets subscriber profile
//   SAA (Server-Assignment-Answer): HSS returns filter criteria + MSISDN + APN
//   LIA/UAR: used by I-CSCF to find which S-CSCF handles this user
//
// OUR SIM: S-CSCF + MTAS combined in one node for simplicity
// ============================================================
class ScscfNode {
public:
    ScscfNode(std::atomic<bool>& stop, std::atomic<bool>& scscf_ready,
              std::atomic<bool>& hss_ready);
    void run();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& scscf_ready_;
    std::atomic<bool>& hss_ready_; // used for startup sync

    Socket server_socket_;
    Socket pcscf_conn_;  // connection from P-CSCF
    Socket hss_conn_;    // Diameter Cx to HSS

    std::map<std::string, ImsSubscriber> registry_; // IMPU → subscriber
    uint32_t next_seq_{1};

    void setupServer();
    void receiveLoop();
    void handleRegister  (const std::vector<uint8_t>& payload);
    void handleInvite    (const std::vector<uint8_t>& payload);
    void handleBye       (const std::vector<uint8_t>& payload);

    void sendCxSAR(const std::string& impu, const std::string& impi);
    bool waitCxSAA(std::string& subscriber_profile);
    void sendSipResponse(SipMsgType type, const std::string& call_id,
                         const std::string& reason, const std::string& sdp = "");
    void invokeMtas(const std::string& impu, const std::string& call_id,
                    const std::string& sdp);
};
