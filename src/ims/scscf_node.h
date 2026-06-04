#pragma once
#include <atomic>
#include <map>
#include <string>
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "ims/sip.h"

struct ImsSubscriber {
    std::string impu;
    std::string impi;
    std::string contact;
    std::string scscf_name;
    bool        registered{false};
};

struct CallState {
    std::string caller_impu;
    std::string callee_impu;
    std::string call_id;
    bool        on_hold{false};
    bool        in_conference{false};
};

// ============================================================
// S-CSCF + MTAS — Serving CSCF + Ericsson Application Server
//
// Multi-UE call routing:
//   Receives INVITE from P-CSCF (originated by UE-A)
//   Invokes MTAS via ISC (service logic check)
//   Routes INVITE to P-CSCF for delivery to callee UE-B
//   Handles re-INVITE (hold, conference, resume)
//   Handles BYE (cleanup, notify MTAS for CDR)
// ============================================================
class ScscfNode {
public:
    ScscfNode(std::atomic<bool>& stop, std::atomic<bool>& scscf_ready,
              std::atomic<bool>& hss_ready);
    void run();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& scscf_ready_;
    std::atomic<bool>& hss_ready_;

    Socket server_socket_;
    Socket pcscf_conn_;
    Socket hss_conn_;

    std::map<std::string, ImsSubscriber> registry_;  // IMPU → subscriber
    std::map<std::string, CallState>     calls_;     // call_id → state
    uint32_t next_seq_{1};

    void setupServer();
    void receiveLoop();
    void handleRegister  (const std::vector<uint8_t>& payload);
    void handleInvite    (const std::vector<uint8_t>& payload);
    void handleReInvite  (const std::vector<uint8_t>& payload, const std::string& call_id,
                          const std::string& from, const std::string& sdp);
    void handleAck       (const std::vector<uint8_t>& payload);
    void handle200Ok     (const std::vector<uint8_t>& payload);
    void handleBye       (const std::vector<uint8_t>& payload);
    void handleUpdate    (const std::vector<uint8_t>& payload);  // SIP UPDATE (RFC 3311)

    void sendCxSAR(const std::string& impu, const std::string& impi);
    bool waitCxSAA(std::string& profile);
    bool invokeMtas(const std::string& caller, const std::string& callee,
                    const std::string& call_id, const std::string& sdp);

    // Route message toward P-CSCF (which delivers to UE)
    void routeToCallee(const std::vector<uint8_t>& payload);
    void routeToCaller(const std::vector<uint8_t>& payload);
    void sendToPcscf(const std::vector<uint8_t>& frame);

    void sendSipResponse(SipMsgType type, const std::string& call_id,
                         const std::string& from, const std::string& to,
                         const std::string& reason, const std::string& sdp = "");
};
