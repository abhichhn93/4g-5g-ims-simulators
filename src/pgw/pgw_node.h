#pragma once
#include <atomic>
#include <mutex>
#include "common/socket_wrapper.h"
#include "common/tlv.h"

class PgwNode {
public:
    PgwNode(std::atomic<bool>& stop, std::atomic<bool>& pgw_ready, std::atomic<bool>& pcrf_ready);
    void run();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& pgw_ready_;
    std::atomic<bool>& pcrf_ready_;

    UdpSocket s5_socket_;
    Socket    pcrf_conn_;       // TCP to PCRF (Diameter Gx)
    bool      pcrf_connected_{false};
    std::mutex pcrf_mutex_;

    std::atomic<uint32_t> next_teid_{2000};
    std::atomic<uint32_t> next_ip_{0x0A000001};  // 10.0.0.1
    uint32_t              next_seq_{1};

    void setupSocket();
    void receiveLoop();
    void handleCreateSessionReq(const std::vector<uint8_t>& payload, const sockaddr_in& sgw_addr);
    bool sendCCRandWaitCCA(uint64_t imsi, const std::string& apn, uint8_t qci);
    std::string allocateIP();
};
