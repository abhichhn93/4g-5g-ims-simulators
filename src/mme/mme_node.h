#pragma once
#include <atomic>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "common/metrics.h"
#include "common/socket_wrapper.h"
#include "common/tlv.h"
#include "mme/ue_context.h"
#include "mme/ue_context_store.h"

// ============================================================
// MME NODE — Phase 3: adds S-GW GTP-C + full attach flow
//
// THREADING MODEL (Phase 3):
// ─────────────────────────────────────────────────────────────
//   mme_th
//     └── MmeNode::run()
//           ├── connectToHSS() + connectToENB()
//           ├── [hss_rx_th] hssReceiveLoop()
//           │     reads AIA → stores in pending_auth_ → notify cv
//           └── enbReceiveLoop()  [mme_th]
//                 handles InitialUEMsg:
//                   → AIR to HSS → cv.wait for AIA
//                   → Auth Request to eNB
//                 handles UL NAS (Auth Response):
//                   → validate RES vs XRES
//                   → handleSessionSetup() [BLOCKING UDP to S-GW]
//                   → Initial Context Setup Request to eNB
//                 handles Initial Context Setup Response:
//                   → Modify Bearer Request [BLOCKING UDP to S-GW]
//                 handles UL NAS (Attach Complete):
//                   → state = REGISTERED ✓
//
// CONCURRENCY PRIMITIVES (Phase 3 additions):
//   UeContextStore: sharded 64-bucket shared_mutex map
//     - Phase 2 used simple std::map + mutex
//     - Phase 3 uses sharded map for reduced lock contention
//     - shared_mutex: concurrent reads, exclusive writes
//   sgw_udp_: S11 UDP socket
//     - blocking recvWithTimeout inside handlers (synchronous for Phase 3)
//     - Phase 4: make async with condition_variable like HSS
// ============================================================
class MmeNode {
public:
    MmeNode(std::atomic<bool>& stop,
            std::atomic<bool>& enb_ready,
            std::atomic<bool>& hss_ready,
            std::atomic<bool>& sgw_ready,
            std::shared_ptr<Metrics> metrics = nullptr);

    void run();
    void printStatus() const;

private:
    std::atomic<bool>& stop_;
    std::atomic<bool>& enb_ready_;
    std::atomic<bool>& hss_ready_;
    std::atomic<bool>& sgw_ready_;

    Socket    enb_conn_;   // TCP: S1AP eNB↔MME
    Socket    hss_conn_;   // TCP: Diameter MME↔HSS
    UdpSocket sgw_udp_;    // UDP: GTP-C S11 MME↔S-GW

    // Sharded UE context store (Phase 3)
    UeContextStore ue_store_;

    // HSS pending auth handoff (Phase 2 pattern, unchanged)
    std::mutex                      pending_auth_mutex_;
    std::condition_variable         pending_auth_cv_;
    std::map<uint64_t, AuthVectors> pending_auth_;

    uint32_t next_seq_{1};
    std::mutex sgw_mtx_;
    std::shared_ptr<Metrics> metrics_;  // null when not doing BULK  // serialize GTP-C sends (Phase 3: single MME thread writes)

    void connectToNodes();
    void enbReceiveLoop();
    void hssReceiveLoop();
    void handleInitialUEMsg   (const std::vector<uint8_t>& payload);
    void handleULNasTransport (const std::vector<uint8_t>& payload);
    void handleICSetupResponse(const std::vector<uint8_t>& payload);
    void handleAuthSuccess    (uint32_t mme_id);
    bool sendCreateSession    (uint32_t mme_id, uint64_t imsi);
};
