#pragma once
#include <atomic>
#include "common/socket_wrapper.h"

// ============================================================
// HSS NODE — Home Subscriber Server
//
// REAL HSS in 4G:
//   - Stores subscriber data: IMSI, Ki (master key), MSISDN, APN profiles
//   - Connected to MME via Diameter S6a (TS 29.272)
//   - Connected to P-CSCF/S-CSCF via Diameter Cx for IMS (Phase 5+)
//   - Can serve thousands of MMEs simultaneously
//   - Typically clustered: active-active with shared DB (Cassandra, Oracle)
//   - In real deployments: HSS is often virtualised (vHSS) as a cloud-native VNF
//
// OUR SIMULATOR:
//   - Single thread, TCP server on port 3868 (real Diameter port!)
//   - Accepts ONE connection from MME (Phase 2 — single MME)
//   - Receives Diameter AIR (Authentication-Information-Request)
//   - Generates simplified auth vectors (not real Milenage)
//   - Sends Diameter AIA (Authentication-Information-Answer)
//
// THREADING:
//   - HssNode::run() runs in hss_th (started from main)
//   - No shared state with other nodes — pure message-in/message-out
//   - Pattern: simplest possible — single thread, synchronous I/O
//
// INTERVIEW Q: "How would you scale HSS for 100M subscribers?"
// ANSWER: "Horizontally scale with consistent hashing — each HSS handles
//   a range of IMSIs. Front-load balancer routes MME requests by IMSI prefix.
//   Store subscriber data in distributed DB (Cassandra). Replicate auth vectors
//   across regions. Real Ericsson/Nokia HSS is a distributed system."
// ============================================================
class HssNode {
public:
    HssNode(std::atomic<bool>& stop, std::atomic<bool>& hss_ready);
    void run();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& hss_ready_;

    Socket server_socket_;
    Socket mme_conn_;

    uint32_t next_seq_{1};

    void setupServer();
    void receiveLoop();
    void handleAIR(const std::vector<uint8_t>& payload, uint32_t req_seq);
    void generateAuthVectors(uint64_t imsi,
                              uint8_t rand_out[16],
                              uint8_t autn_out[16],
                              uint8_t xres_out[8],
                              uint8_t kasme_out[32]);
};
