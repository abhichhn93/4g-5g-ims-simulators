#include "pcrf/pcrf_node.h"
#include "common/logger.h"
#include "common/message_types.h"
#include "common/subscriber_profile.h"
#include "common/pcap_writer.h"
#include <stdexcept>

static constexpr const char* PCRF_IP   = "127.0.0.1";
static constexpr uint16_t    PCRF_PORT  = 3869;

PcrfNode::PcrfNode(std::atomic<bool>& stop, std::atomic<bool>& pcrf_ready)
    : stop_(stop), pcrf_ready_(pcrf_ready)
{}

void PcrfNode::run() {
    Logger::pcrf("PCRF: thread started");
    try {
        setupServer();
        if (!stop_.load()) receiveLoop();
    } catch (const std::exception& e) {
        Logger::warn("PCRF", e.what());
    }
    Logger::pcrf("PCRF: thread exiting");
}

void PcrfNode::setupServer() {
    Logger::pcrf("PCRF: TCP server on " + std::string(PCRF_IP) + ":" + std::to_string(PCRF_PORT));
    Logger::pcrf("PCRF: REAL: Diameter Gx uses port 3868 (same as S6a) with Application-ID=16777238");
    Logger::pcrf("PCRF: OUR SIM: separate port 3869 for Gx to keep it visually distinct from HSS");
    Logger::pcrf("PCRF: REAL: PCRF is often co-located with HSS in small deployments");

    server_socket_ = Socket::createServer(PCRF_IP, PCRF_PORT);
    pcrf_ready_.store(true);
    Logger::pcrf("PCRF: ready ✓ — waiting for P-GW to connect...");

    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            pgw_conn_ = server_socket_.accept();
            Logger::pcrf("PCRF: P-GW connected — Gx link UP ✓");
            return;
        }
    }
}

void PcrfNode::receiveLoop() {
    Logger::pcrf("PCRF: entering receive loop");
    while (!stop_.load()) {
        if (!pgw_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!pgw_conn_.recvFrame(payload)) {
            Logger::pcrf("PCRF: P-GW disconnected");
            break;
        }
        if (payload.size() < 8) continue;

        MessageReader r(payload);
        if (r.msgType() == MessageType::DIA_GX_CCR_I) {
            handleCCR(payload);
        } else {
            Logger::warn("PCRF", "unexpected msg: " + std::string(msg_type_str(r.msgType())));
        }
    }
}

void PcrfNode::handleCCR(const std::vector<uint8_t>& payload) {
    uint64_t    imsi = 0;
    std::string apn;
    uint8_t     qci  = 9;
    (void)qci;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len;
        if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::GX_IMSI: imsi = r.readU64(); break;
            case Tag::GX_APN:  apn  = r.readStr(); break;
            case Tag::GX_QCI:  qci  = r.readU8();  break;
            default:           r.skip();            break;
        }
    }

    Logger::pcrf("PCRF: ← RECV Diameter Gx CCR-I [TS 29.212 §4.5.1]");
    Logger::ie_field("  IMSI=" + std::to_string(imsi) + "  APN=" + apn);
    Logger::pcrf("PCRF: REAL: PCRF checks subscriber policy from subscriber database:");
    Logger::pcrf("PCRF:   Is this IMSI allowed on APN '" + apn + "'?");
    Logger::pcrf("PCRF:   What QoS policy applies? Any special service triggers?");
    Logger::pcrf("PCRF:   Is online charging required? (pre-paid subscribers)");

    // Look up the subscriber's profile (Flyweight — same shared object for all "internet" UEs)
    auto profile = ProfileRegistry::instance().get(apn.empty() ? "internet" : apn);

    Logger::pcrf("PCRF: Flyweight: all '" + profile->apn + "' UEs share profile@" +
                [&]{ char b[32]; std::snprintf(b,32,"%p",static_cast<const void*>(profile.get())); return std::string(b); }());
    Logger::ie_field("  Profile: APN=" + profile->apn +
                     "  QCI=" + std::to_string(profile->qci) +
                     "  MaxUL=" + std::to_string(profile->max_ul_bps/1000000) + "Mbps" +
                     "  MaxDL=" + std::to_string(profile->max_dl_bps/1000000) + "Mbps");
    Logger::ie_field("  Charging rule: '" + profile->charging_rule + "'");
    Logger::pcrf("PCRF: → SEND Diameter Gx CCA-I — policy approved");
    // PCAP: CCR received (P-GW→PCRF) then CCA sent (PCRF→P-GW)
    PcapWriter::instance().writeDiameter(
        DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, true,
        PcapWriter::IP_PGW, PcapWriter::PORT_GX,
        PcapWriter::IP_PCRF, PcapWriter::PORT_GX);
    PcapWriter::instance().writeDiameter(
        DiameterCmd::CREDIT_CONTROL, DiameterApp::GX, false,
        PcapWriter::IP_PCRF, PcapWriter::PORT_GX,
        PcapWriter::IP_PGW, PcapWriter::PORT_GX);

    // Build CCA-I response with charging rules
    MessageWriter rsp(MessageType::DIA_GX_CCA_I, next_seq_++);
    rsp.writeU8 (Tag::GX_RESULT_CODE, 0x01);          // 1 = DIAMETER_SUCCESS (simplified)
    rsp.writeU64(Tag::GX_IMSI,        imsi);
    rsp.writeStr(Tag::GX_APN,         profile->apn);
    rsp.writeU8 (Tag::GX_QCI,         profile->qci);
    rsp.writeU32(Tag::GX_MAX_UL_BPS,  profile->max_ul_bps);
    rsp.writeU32(Tag::GX_MAX_DL_BPS,  profile->max_dl_bps);
    rsp.writeStr(Tag::GX_RULE_NAME,   profile->charging_rule);
    pgw_conn_.sendFrame(rsp.frame());

    Logger::ie_field("  CCA-I: rule='" + profile->charging_rule +
                     "' QCI=" + std::to_string(profile->qci) + " approved");
}
