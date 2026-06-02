#pragma once
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include "common/socket_wrapper.h"
#include "common/tlv.h"

class EnbNode {
public:
    EnbNode(std::atomic<bool>& stop, std::atomic<bool>& enb_ready);
    void run();
    void submitCommand(const std::string& cmd);
    void requestStop();

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& enb_ready_;

    std::queue<std::string> cmd_queue_;
    std::mutex              cmd_mutex_;
    std::condition_variable cmd_cv_;

    Socket   server_socket_;
    Socket   mme_conn_;
    std::mutex mme_send_mtx_;  // protects concurrent writes from commandLoop + receiveLoop

    std::atomic<uint32_t> next_enb_teid_{100};  // eNB S1-U TEIDs start at 100
    uint32_t next_enb_ue_id_{1};
    uint32_t next_seq_{1};

    void setupServer();
    void commandLoop();
    void receiveLoop();
    void processCommand(const std::string& cmd);
    void sendInitialUEMessage(uint32_t ue_index);
    void handleDLNas(const std::vector<uint8_t>& payload);
    void handleICSR (const std::vector<uint8_t>& payload);
};
