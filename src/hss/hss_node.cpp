#include "hss/hss_node.h"
#include "common/tlv.h"
#include "common/logger.h"
#include "common/pcap_writer.h"
#include <random>
#include <stdexcept>

static constexpr const char* HSS_IP   = "127.0.0.1";
static constexpr uint16_t    HSS_PORT = 3868;  // real Diameter port

HssNode::HssNode(std::atomic<bool>& stop, std::atomic<bool>& hss_ready)
    : stop_(stop), hss_ready_(hss_ready)
{}

void HssNode::run() {
    Logger::hss("HSS: thread started");
    try {
        setupServer();
        if (!stop_.load()) receiveLoop();
    } catch (const std::exception& e) {
        Logger::warn("HSS", e.what());
    }
    Logger::hss("HSS: thread exiting");
}

void HssNode::setupServer() {
    Logger::hss("HSS: creating TCP server on " + std::string(HSS_IP) + ":" + std::to_string(HSS_PORT));
    Logger::hss("HSS: REAL: Diameter S6a uses SCTP on port 3868 (TS 29.272). We use TCP.");
    Logger::hss("HSS: REAL: Multiple MMEs connect simultaneously. We accept 1 (Phase 2).");

    server_socket_ = Socket::createServer(HSS_IP, HSS_PORT);
    hss_ready_.store(true);

    Logger::hss("HSS: listening — waiting for MME to connect...");
    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            mme_conn_ = server_socket_.accept();
            Logger::hss("HSS: MME connected ✓ — Diameter S6a link UP");
            return;
        }
    }
}

void HssNode::receiveLoop() {
    Logger::hss("HSS: entering receive loop — waiting for Diameter AIR from MME");

    while (!stop_.load()) {
        if (!mme_conn_.hasData(100)) continue;

        std::vector<uint8_t> payload;
        if (!mme_conn_.recvFrame(payload)) {
            Logger::hss("HSS: MME disconnected — exiting receive loop");
            break;
        }

        MessageReader r(payload);
        if (r.msgType() == MessageType::DIA_AIR) {
            handleAIR(payload, r.seqNum());
        } else {
            Logger::warn("HSS", "unexpected message type: " +
                         std::string(msg_type_str(r.msgType())));
        }
    }
}

void HssNode::handleAIR(const std::vector<uint8_t>& payload, uint32_t req_seq) {
    MessageReader r(payload);

    // Parse AIR TLVs
    uint64_t imsi  = 0;
    uint32_t plmn  = 0;
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::DIA_IMSI: imsi = r.readU64(); break;
            case Tag::DIA_PLMN: plmn = r.readU32(); break;
            default:            r.skip(); break;  // unknown tag — skip gracefully
        }
    }

    Logger::hss("HSS: ← RECV Diameter AIR [TS 29.272 §5.2.3.1] seq=" + std::to_string(req_seq));
    Logger::ie_field("  IMSI = " + std::to_string(imsi));
    Logger::ie_field("  Visited-PLMN = " + std::to_string(plmn));
    Logger::hss("HSS:   → Looking up subscriber record...");
    Logger::hss("HSS:   → REAL: HSS looks up Ki for this IMSI in subscriber DB");
    Logger::hss("HSS:   → REAL: Runs Milenage (f1/f2/f3/f4/f5) with Ki + fresh RAND");
    Logger::hss("HSS:   → OUR SIM: Generating simplified auth vectors");

    // Generate auth vectors
    uint8_t rand_v[16], autn_v[16], xres_v[8], kasme_v[32];
    generateAuthVectors(imsi, rand_v, autn_v, xres_v, kasme_v);

    // Log what we generated
    Logger::ie_field("  RAND  = [16 random bytes] — challenge to UE");
    Logger::ie_field("  AUTN  = RAND XOR 0xAA (simplified) — UE verifies network with this");
    Logger::ie_field("  XRES  = RAND[0..7] XOR 0x55 — MME will compare UE's RES with this");
    Logger::ie_field("  Kasme = zeros (Phase 3 will use real HKDF from Ki+RAND)");
    Logger::hss("HSS:   → Sending AIA (auth vectors) back to MME...");

    // Build and send AIA
    MessageWriter w(MessageType::DIA_AIA, next_seq_++);
    w.writeU64(Tag::DIA_IMSI, imsi);
    w.writeBytes(Tag::DIA_RAND,  rand_v,  16);
    w.writeBytes(Tag::DIA_AUTN,  autn_v,  16);
    w.writeBytes(Tag::DIA_XRES,  xres_v,   8);
    w.writeBytes(Tag::DIA_KASME, kasme_v, 32);

    if (mme_conn_.sendFrame(w.frame())) {
        Logger::hss("HSS: → SEND Diameter AIA [TS 29.272 §5.2.3.2] — auth vectors delivered");
        PcapWriter::instance().writeDiameter(
            DiameterCmd::AUTH_INFO, DiameterApp::S6A, false,
            PcapWriter::IP_HSS, PcapWriter::PORT_DIA,
            PcapWriter::IP_MME, PcapWriter::PORT_DIA);
    } else {
        Logger::warn("HSS", "failed to send AIA");
    }
}

void HssNode::generateAuthVectors(uint64_t imsi,
                                   uint8_t rand_out[16],
                                   uint8_t autn_out[16],
                                   uint8_t xres_out[8],
                                   uint8_t kasme_out[32]) {
    // REAL: Milenage algorithm (AES-based f1/f2/f3/f4/f5 functions)
    //   f1  → MAC-A (used in AUTN)
    //   f2  → RES  (response, UE computes same value using SIM's Ki)
    //   f3  → CK   (cipher key)
    //   f4  → IK   (integrity key)
    //   f5  → AK   (anonymity key, XORed with SQN in AUTN)
    //   Kasme is then derived from CK/IK using KDF (TS 33.401 Annex A)
    //
    // OUR SIM: use mt19937 seeded with IMSI for reproducibility
    std::mt19937 rng(static_cast<uint32_t>(imsi ^ (imsi >> 32)));

    // RAND: 16 random bytes
    for (int i = 0; i < 16; i++) rand_out[i] = static_cast<uint8_t>(rng());

    // AUTN: RAND XOR 0xAA (simplified — no real Milenage)
    for (int i = 0; i < 16; i++) autn_out[i] = rand_out[i] ^ 0xAA;

    // XRES: first 8 bytes of (RAND XOR 0x55)
    // UE will compute the same RES = RAND XOR 0x55 and send it back.
    // MME compares UE's RES with this XRES. Match = auth success.
    for (int i = 0; i < 8; i++) xres_out[i] = rand_out[i] ^ 0x55;

    // Kasme: all zeros for now (Phase 3 will derive from CK/IK using HKDF)
    std::memset(kasme_out, 0, 32);
}
