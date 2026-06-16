#include "enb/enb_node.h"
#include "common/logger.h"
#include "common/pcap_writer.h"
#include "common/nas_eps.h"
#include "common/s1ap_codec.h"
#include <stdexcept>
#include <cstdio>
#include <thread>
#include <chrono>

static constexpr const char* ENB_IP  = "127.0.0.1";
static constexpr uint16_t    ENB_PORT = 38412;
static constexpr uint64_t    BASE_IMSI = 404100000000000ULL;

EnbNode::EnbNode(std::atomic<bool>& stop, std::atomic<bool>& enb_ready)
    : stop_(stop), enb_ready_(enb_ready)
{}

void EnbNode::run() {
    Logger::enb(Logger::Level::ENGINEER, "thread started");
    try {
        setupServer();
        if (stop_.load()) return;
        std::thread rx_th([this]{ receiveLoop(); });
        commandLoop();
        rx_th.join();
    } catch (const std::exception& e) {
        Logger::warn("eNB", e.what());
    }
    Logger::enb(Logger::Level::ENGINEER, "thread exiting");
}

void EnbNode::setupServer() {
    Logger::enb(Logger::Level::ENGINEER, "TCP server on " + std::string(ENB_IP) + ":" + std::to_string(ENB_PORT));
    server_socket_ = Socket::createServer(ENB_IP, ENB_PORT);
    enb_ready_.store(true);
    Logger::enb(Logger::Level::ENGINEER, "listening — waiting for MME...");
    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            mme_conn_ = server_socket_.accept();
            Logger::enb(Logger::Level::ENGINEER, "MME connected — S1 UP ✓");
            return;
        }
    }
}

void EnbNode::receiveLoop() {
    Logger::enb(Logger::Level::ENGINEER, "[rx_th] started — handles DL NAS + ICSR from MME");
    while (!stop_.load()) {
        if (!mme_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!mme_conn_.recvFrame(payload)) break;
        if (payload.size() < 8) continue;
        MessageReader r(payload);
        switch (r.msgType()) {
            case MessageType::S1AP_DL_NAS_TRANSPORT:          handleDLNas(payload);       break;
            case MessageType::S1AP_INITIAL_CONTEXT_SETUP_REQ: handleICSR(payload);        break;
            case MessageType::S1AP_TAU_ACCEPT:                handleTauAccept(payload);   break;
            case MessageType::S1AP_HANDOVER_REQUEST:          handleHoRequest(payload);   break;
            case MessageType::S1AP_HANDOVER_COMMAND:          handleHoCommand(payload);   break;
            case MessageType::S1AP_MME_STATUS_TRANSFER:       handleMmeStatusXfer(payload); break;
            case MessageType::S1AP_UE_CONTEXT_RELEASE_CMD:    handleUeCtxRelCmd(payload); break;
            default: Logger::warn("eNB","[rx_th] unknown: "+std::string(msg_type_str(r.msgType())));
        }
    }
    Logger::enb(Logger::Level::ENGINEER, "[rx_th] stopped");
}

void EnbNode::handleDLNas(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0; uint8_t nas_type=0;
    std::vector<uint8_t> rand_b, autn_b;

    MessageReader r(payload);
    while(r.hasMore()) {
        Tag tag; uint16_t len; if(!r.peek(tag,len)) break;
        switch(tag) {
            case Tag::MME_UE_S1AP_ID: mme_id=r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id=r.readU32(); break;
            case Tag::NAS_MSG_TYPE:   nas_type=r.readU8(); break;
            case Tag::NAS_RAND:       rand_b=r.readBytes(); break;
            case Tag::NAS_AUTN:       autn_b=r.readBytes(); break;
            default: r.skip(); break;
        }
    }

    Logger::enb(Logger::Level::ENGINEER, "[rx_th] ← DL NAS Transport  nas_type=0x" + [&]{ char b[8]; std::snprintf(b,8,"%02X",nas_type); return std::string(b); }());
    // PCAP: S1AP DL NAS Transport (MME→eNB)
    if (nas_type == 0x52 && rand_b.size() >= 16 && autn_b.size() >= 16) {
        auto nas_pdu = nas_eps::buildAuthRequest(rand_b.data(), autn_b.data());
        PcapWriter::instance().writeS1AP("NAS-AuthRequest(DL)",
            PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
            PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
            s1ap::buildDlNasTransport(mme_id, enb_id, nas_pdu));
    } else if (nas_type == 0x5D) {
        PcapWriter::instance().writeS1AP("NAS-SecurityModeCmd(DL)",
            PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
            PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
            s1ap::buildDlNasTransport(mme_id, enb_id, nas_eps::SECURITY_MODE_COMMAND));
    }

    if (nas_type == 0x5D) {
        // Security Mode Command received — UE activates algorithms, sends Complete
        Logger::enb(Logger::Level::ENGINEER, "[rx_th] ← NAS Security Mode Command (0x5D)");
        Logger::ie_field("  Cipher: EEA2 (AES-128-CTR), Integrity: EIA2 (AES-128-CMAC)");
        Logger::ie_field("  UE derives KNASenc + KNASint from KASME");
        Logger::ie_field("  REAL: all subsequent NAS messages are integrity-protected + ciphered");
        Logger::enb(Logger::Level::ENGINEER, "[rx_th] → NAS Security Mode Complete (0x5E)  UE activated security");

        std::lock_guard<std::mutex> lk(mme_send_mtx_);
        MessageWriter smc_complete(MessageType::S1AP_UL_NAS_TRANSPORT, next_seq_++);
        smc_complete.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
        smc_complete.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
        smc_complete.writeU8 (Tag::NAS_MSG_TYPE,   0x5E);  // Security Mode Complete
        mme_conn_.sendFrame(smc_complete.frame());
        PcapWriter::instance().writeS1AP("NAS-SecurityModeComplete(UL)",
            PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
            PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
            s1ap::buildUlNasTransport(mme_id, enb_id, nas_eps::SECURITY_MODE_COMPLETE));
        return;
    }

    if (nas_type == 0x52 && rand_b.size() >= 8) {
        uint8_t res[8]; for(int i=0;i<8;i++) res[i]=rand_b[i]^0x55;
        Logger::enb(Logger::Level::ENGINEER, "[rx_th] SIM: UE computes RES=RAND^0x55 — sending Auth Response");
        std::lock_guard<std::mutex> lk(mme_send_mtx_);
        MessageWriter ul(MessageType::S1AP_UL_NAS_TRANSPORT, next_seq_++);
        ul.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
        ul.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
        ul.writeU8 (Tag::NAS_MSG_TYPE,   0x53);
        ul.writeBytes(Tag::NAS_RES, res, 8);
        mme_conn_.sendFrame(ul.frame());
        // PCAP: S1AP UL NAS Transport (eNB→MME) — Auth Response
        PcapWriter::instance().writeS1AP("NAS-AuthResponse(UL)",
            PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
            PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
            s1ap::buildUlNasTransport(mme_id, enb_id, nas_eps::buildAuthResponse(res)));
    }
}

void EnbNode::handleICSR(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0, sgw_teid=0;
    std::vector<uint8_t> ue_ip_bytes;
    uint8_t nas_type=0;

    MessageReader r(payload);
    while(r.hasMore()) {
        Tag tag; uint16_t len; if(!r.peek(tag,len)) break;
        switch(tag) {
            case Tag::MME_UE_S1AP_ID: mme_id    = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id    = r.readU32(); break;
            case Tag::ICSR_SGW_TEID:  sgw_teid  = r.readU32(); break;
            case Tag::NAS_MSG_TYPE:   nas_type   = r.readU8();  break;
            case Tag::NAS_UE_IP:      ue_ip_bytes= r.readBytes(); break;
            default: r.skip(); break;
        }
    }

    char ue_ip[32]="?";
    if (ue_ip_bytes.size()>=4)
        std::snprintf(ue_ip,32,"%d.%d.%d.%d",ue_ip_bytes[0],ue_ip_bytes[1],ue_ip_bytes[2],ue_ip_bytes[3]);

    Logger::enb(Logger::Level::ENGINEER, "[rx_th] ← S1AP InitialContextSetupRequest [TS 36.413 §9.1.4.1]");
    Logger::ie_field("  MME-id=" + std::to_string(mme_id) + "  S-GW S1U-TEID=" + std::to_string(sgw_teid));
    Logger::ie_field("  NAS: Attach Accept (0x42)  UE-IP=" + std::string(ue_ip));
    Logger::enb(Logger::Level::ENGINEER, "[rx_th] REAL: eNB sets up DRB (Data Radio Bearer) for UE");
    Logger::enb(Logger::Level::ENGINEER, "[rx_th] REAL: eNB creates GTP-U tunnel to S-GW using S-GW's S1U TEID");
    Logger::enb(Logger::Level::ENGINEER, "[rx_th] SIM: allocating eNB's S1-U TEID for the uplink tunnel");

    uint32_t enb_teid = next_enb_teid_.fetch_add(1);

    // PCAP: S1AP Initial Context Setup Request (MME→eNB)
    uint8_t ue_ip4[4] = {0, 0, 0, 0};
    if (ue_ip_bytes.size() >= 4) for (int i = 0; i < 4; ++i) ue_ip4[i] = ue_ip_bytes[i];
    // No real KeNB exists in the simulator (no AS security derivation) — derive a
    // deterministic 32-byte SecurityKey per (eNB,MME) pair for cosmetic realism.
    uint8_t kenb[32];
    for (int i = 0; i < 32; ++i) kenb[i] = static_cast<uint8_t>((enb_id * 31 + mme_id * 17 + i * 7) & 0xFF);
    PcapWriter::instance().writeS1AP("S1AP-InitialContextSetupReq",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildInitialContextSetupRequest(mme_id, enb_id, sgw_teid, ue_ip4, kenb));
    Logger::enb(Logger::Level::ENGINEER, "[rx_th] → S1AP InitialContextSetupResponse  eNB S1-U TEID=" + std::to_string(enb_teid));
    { std::lock_guard<std::mutex> lk(mme_send_mtx_);
      MessageWriter rsp(MessageType::S1AP_INITIAL_CONTEXT_SETUP_RSP, next_seq_++);
      rsp.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
      rsp.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
      rsp.writeU32(Tag::ICSR_ENB_TEID,  enb_teid);
      mme_conn_.sendFrame(rsp.frame());
      PcapWriter::instance().writeS1AP("S1AP-InitialContextSetupRsp",
          PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
          PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
          s1ap::buildInitialContextSetupResponse(mme_id, enb_id, enb_teid)); }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Logger::enb(Logger::Level::ENGINEER, "[rx_th] SIM: UE sends NAS Attach Complete (0x46)");
    { std::lock_guard<std::mutex> lk(mme_send_mtx_);
      MessageWriter ac(MessageType::S1AP_UL_NAS_TRANSPORT, next_seq_++);
      ac.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
      ac.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
      ac.writeU8 (Tag::NAS_MSG_TYPE,   0x46);
      mme_conn_.sendFrame(ac.frame());
      PcapWriter::instance().writeS1AP("NAS-AttachComplete(UL)",
          PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
          PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
          s1ap::buildUlNasTransport(mme_id, enb_id, nas_eps::ATTACH_COMPLETE)); }
}

void EnbNode::commandLoop() {
    Logger::enb(Logger::Level::ENGINEER, "[enb_th] command loop started");
    while (!stop_.load()) {
        std::unique_lock<std::mutex> lk(cmd_mutex_);
        cmd_cv_.wait(lk,[this]{ return !cmd_queue_.empty()||stop_.load(); });
        while (!cmd_queue_.empty()) {
            std::string cmd=cmd_queue_.front(); cmd_queue_.pop();
            lk.unlock(); processCommand(cmd); lk.lock();
        }
    }
    Logger::enb(Logger::Level::ENGINEER, "[enb_th] command loop stopped");
}

void EnbNode::processCommand(const std::string& cmd) {
    if (cmd.size()>=2 && cmd.substr(0,2)=="CR") {
        int n=1; try{if(cmd.size()>3) n=std::stoi(cmd.substr(3));}catch(...){}
        for(int i=0;i<n&&!stop_.load();++i) sendInitialUEMessage(next_enb_ue_id_++);
    } else if (cmd.size()>=3 && cmd.substr(0,3)=="TAU") {
        // TAU <ue_id>  — trigger Tracking Area Update for an attached UE
        uint32_t ue_id=1; try{if(cmd.size()>4) ue_id=std::stoi(cmd.substr(4));}catch(...){}
        uint64_t imsi = BASE_IMSI + ue_id;
        Logger::enb(Logger::Level::ENGINEER, "[cmd] TAU triggered for UE " + std::to_string(ue_id));
        sendTauRequest(ue_id, ue_id, imsi);  // mme_id≈enb_id for single-UE sim
    } else if (cmd.size()>=2 && cmd.substr(0,2)=="HO") {
        // HO <ue_id>  — trigger S1 Handover for an attached UE
        uint32_t ue_id=1; try{if(cmd.size()>3) ue_id=std::stoi(cmd.substr(3));}catch(...){}
        Logger::enb(Logger::Level::ENGINEER, "[cmd] HO triggered for UE " + std::to_string(ue_id));
        sendHandoverRequired(ue_id, ue_id);
    }
}

void EnbNode::sendInitialUEMessage(uint32_t ue_index) {
    if (!mme_conn_.valid()) return;
    uint64_t imsi = BASE_IMSI + ue_index;
    Logger::enb(Logger::Level::ENGINEER, "[enb_th] → InitialUEMessage  eNB-id=" + std::to_string(ue_index) + " IMSI=" + std::to_string(imsi));
    std::lock_guard<std::mutex> lk(mme_send_mtx_);
    MessageWriter w(MessageType::S1AP_INITIAL_UE_MSG, next_seq_++);
    w.writeU32(Tag::ENB_UE_S1AP_ID,  ue_index);
    w.writeU8 (Tag::RRC_CAUSE,        3);
    w.writeU16(Tag::TAI_MCC,          404);
    w.writeU16(Tag::TAI_MNC,          10);
    w.writeU16(Tag::TAI_TAC,          1);
    w.writeU32(Tag::EUTRAN_CGI,       1);
    w.writeU8 (Tag::NAS_PROTO_DISC,   0x07);
    w.writeU8 (Tag::NAS_SEC_HDR,      0x00);
    w.writeU8 (Tag::NAS_MSG_TYPE,     0x41);
    w.writeU8 (Tag::NAS_ATTACH_TYPE,  1);
    w.writeU8 (Tag::NAS_KSI,          7);
    w.writeU8 (Tag::NAS_ID_TYPE,      1);
    w.writeU64(Tag::NAS_IMSI,         imsi);
    w.writeU8 (Tag::NAS_UE_CAP,       0xE0);
    mme_conn_.sendFrame(w.frame());
    // PCAP: S1AP Initial UE Message = Attach Request + PDN Connectivity Request
    PcapWriter::instance().writeS1AP("S1AP-InitialUEMsg(AttachReq)",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildInitialUEMessage(ue_index, imsi));
}

void EnbNode::submitCommand(const std::string& cmd) {
    { std::lock_guard<std::mutex> lk(cmd_mutex_); cmd_queue_.push(cmd); }
    cmd_cv_.notify_one();
}
void EnbNode::requestStop() { cmd_cv_.notify_all(); }

// ═══════════════════════════════════════════════════════════
// TAU — eNB-side sender and accept handler
// ═══════════════════════════════════════════════════════════
void EnbNode::sendTauRequest(uint32_t mme_id, uint32_t enb_id, uint64_t imsi) {
    if (!mme_conn_.valid()) { Logger::warn("eNB","TAU: no MME connection"); return; }

    Logger::enb(Logger::Level::ENGINEER,
        "[TAU] → TAU Request  IMSI=" + std::to_string(imsi) + " (simulating UE moved to new TAI)");
    Logger::ie_field("  BEGINNER: UE tells MME it moved to a new Tracking Area.");
    Logger::ie_field("  INTERVIEW: Periodic TAU: UE also sends even without moving (T3412 timer).");

    // PCAP: UL NAS TAU Request wrapped in S1AP UL NAS Transport
    PcapWriter::instance().writeS1AP("NAS-TauReq(UL)",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildUlNasTransport(mme_id, enb_id, nas_eps::buildTauRequest(imsi)));

    std::lock_guard<std::mutex> lk(mme_send_mtx_);
    MessageWriter w(MessageType::S1AP_TAU_REQUEST, next_seq_++);
    w.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    w.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    w.writeU64(Tag::TAU_IMSI,       imsi);
    w.writeU8 (Tag::TAU_UPDATE_TYPE, 0x00);  // TA updating
    mme_conn_.sendFrame(w.frame());
}

void EnbNode::handleTauAccept(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id = r.readU32(); break;
            default: r.skip(); break;
        }
    }
    Logger::enb(Logger::Level::ENGINEER, "[TAU] ← TAU Accept — UE context updated ✓");
    Logger::ie_field("  BEGINNER: Network confirmed the UE's new location. Tracking area updated.");
    Logger::ie_field("  INTERVIEW: TAU Accept carries new TAI list so UE won't TAU again in same area.");

    PcapWriter::instance().writeS1AP("NAS-TauAccept(DL)",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildDlNasTransport(mme_id, enb_id, nas_eps::buildTauAccept()));
    Logger::sys("TAU COMPLETE: UE successfully updated to new Tracking Area.");
}

// ═══════════════════════════════════════════════════════════
// S1 HANDOVER — eNB-side sender and handlers (TS 36.413 §8.4)
// ═══════════════════════════════════════════════════════════
void EnbNode::sendHandoverRequired(uint32_t mme_id, uint32_t enb_id) {
    if (!mme_conn_.valid()) { Logger::warn("eNB","HO: no MME connection"); return; }

    Logger::enb(Logger::Level::ENGINEER,
        "[HO] Step1 → HandoverRequired  (RRM decided: A3 event triggered)");
    Logger::ie_field("  BEGINNER: eNB tells MME 'I want to hand this UE to another cell'.");
    Logger::ie_field("  INTERVIEW: A3 event: RSRP(neighbor) - RSRP(serving) > threshold.");
    Logger::ie_field("  S1 HO used when NO X2 link between eNBs or inter-MME required.");

    PcapWriter::instance().writeS1AP("S1AP-HandoverRequired",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverRequired(mme_id, enb_id));

    std::lock_guard<std::mutex> lk(mme_send_mtx_);
    MessageWriter w(MessageType::S1AP_HANDOVER_REQUIRED, next_seq_++);
    w.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    w.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    w.writeU64(Tag::TAU_IMSI,       BASE_IMSI + mme_id);
    w.writeU8 (Tag::HO_TYPE,  0);   // intralte
    w.writeU8 (Tag::HO_CAUSE, 0);   // radioNetwork:handover-desirable
    mme_conn_.sendFrame(w.frame());
}

void EnbNode::handleHoRequest(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id = r.readU32(); break;
            default: r.skip(); break;
        }
    }
    Logger::enb(Logger::Level::ENGINEER, "[HO] Step2 ← HandoverRequest (target eNB: allocating resources)");
    Logger::ie_field("  BEGINNER: MME asked the target cell to prepare for the UE.");
    Logger::ie_field("  INTERVIEW: Target eNB sets up DRB, creates uplink GTP-U endpoint.");

    PcapWriter::instance().writeS1AP("S1AP-HandoverRequest",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverRequest(mme_id, enb_id));

    uint32_t new_teid = next_enb_teid_.fetch_add(1);
    Logger::enb(Logger::Level::ENGINEER,
        "[HO] Step3 → HandoverRequestAck  new eNB TEID=" + std::to_string(new_teid));

    PcapWriter::instance().writeS1AP("S1AP-HandoverRequestAck",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverRequestAck(mme_id, enb_id));

    std::lock_guard<std::mutex> lk(mme_send_mtx_);
    MessageWriter ack(MessageType::S1AP_HANDOVER_REQUEST_ACK, next_seq_++);
    ack.writeU32(Tag::MME_UE_S1AP_ID,  mme_id);
    ack.writeU32(Tag::ENB_UE_S1AP_ID,  enb_id);
    ack.writeU32(Tag::HO_ENB_TEID_NEW, new_teid);
    mme_conn_.sendFrame(ack.frame());
}

void EnbNode::handleHoCommand(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id = r.readU32(); break;
            default: r.skip(); break;
        }
    }
    Logger::enb(Logger::Level::ENGINEER, "[HO] Step4 ← HandoverCommand — forwarding RRC reconfig to UE");
    Logger::ie_field("  BEGINNER: eNB tells UE 'disconnect from me, go connect to the target cell'.");
    Logger::ie_field("  INTERVIEW: UE goes into RRC HO Execution — random access to target cell.");

    PcapWriter::instance().writeS1AP("S1AP-HandoverCommand",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverCommand(mme_id, enb_id));

    // UE completes: send ENBStatusTransfer then HandoverNotify from "target" eNB
    std::this_thread::sleep_for(std::chrono::milliseconds(20));

    Logger::enb(Logger::Level::ENGINEER, "[HO] Step5 → ENBStatusTransfer  PDCP SN state");
    PcapWriter::instance().writeS1AP("S1AP-ENBStatusTransfer",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildENBStatusTransfer(mme_id, enb_id));

    {
        std::lock_guard<std::mutex> lk(mme_send_mtx_);
        MessageWriter est(MessageType::S1AP_ENB_STATUS_TRANSFER, next_seq_++);
        est.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
        est.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
        est.writeU32(Tag::HO_PDCP_SN_UL, 42);
        est.writeU32(Tag::HO_PDCP_SN_DL, 37);
        mme_conn_.sendFrame(est.frame());
    }
}

void EnbNode::handleMmeStatusXfer(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0, pdcp_ul=0, pdcp_dl=0;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id  = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id  = r.readU32(); break;
            case Tag::HO_PDCP_SN_UL:  pdcp_ul = r.readU32(); break;
            case Tag::HO_PDCP_SN_DL:  pdcp_dl = r.readU32(); break;
            default: r.skip(); break;
        }
    }
    Logger::enb(Logger::Level::ENGINEER,
        "[HO] Step5b ← MMEStatusTransfer  UL_SN=" + std::to_string(pdcp_ul)
        + " DL_SN=" + std::to_string(pdcp_dl));
    Logger::ie_field("  BEGINNER: MME forwarded the PDCP state so target eNB can reorder packets.");
    Logger::ie_field("  INTERVIEW: This enables lossless HO — no packet loss during radio switch.");

    PcapWriter::instance().writeS1AP("S1AP-MMEStatusTransfer",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildMMEStatusTransfer(mme_id, enb_id));

    // Target eNB got PDCP state → UE attached to target → send HandoverNotify
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    Logger::enb(Logger::Level::ENGINEER, "[HO] Step6 → HandoverNotify  UE is now in target cell ✓");

    PcapWriter::instance().writeS1AP("S1AP-HandoverNotify",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverNotify(mme_id, enb_id));

    std::lock_guard<std::mutex> lk(mme_send_mtx_);
    MessageWriter notify(MessageType::S1AP_HANDOVER_NOTIFY, next_seq_++);
    notify.writeU32(Tag::MME_UE_S1AP_ID,  mme_id);
    notify.writeU32(Tag::ENB_UE_S1AP_ID,  enb_id);
    notify.writeU32(Tag::HO_ENB_TEID_NEW, next_enb_teid_.load() - 1);
    mme_conn_.sendFrame(notify.frame());
}

void EnbNode::handleUeCtxRelCmd(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id = r.readU32(); break;
            default: r.skip(); break;
        }
    }
    Logger::enb(Logger::Level::ENGINEER,
        "[HO] Step7 ← UEContextReleaseCommand — releasing source resources");
    Logger::ie_field("  BEGINNER: MME tells old eNB 'the UE has gone, release your resources'.");
    Logger::ie_field("  INTERVIEW: After this, old eNB TEID is freed. Data flows only via target.");

    PcapWriter::instance().writeS1AP("S1AP-UEContextReleaseCmd",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildUEContextReleaseCommand(mme_id, enb_id));

    PcapWriter::instance().writeS1AP("S1AP-UEContextReleaseCmpl",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildUEContextReleaseComplete(mme_id, enb_id));

    std::lock_guard<std::mutex> lk(mme_send_mtx_);
    MessageWriter cmpl(MessageType::S1AP_UE_CONTEXT_RELEASE_CMPL, next_seq_++);
    cmpl.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    cmpl.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    mme_conn_.sendFrame(cmpl.frame());

    Logger::sys("S1 HANDOVER COMPLETE: UE seamlessly moved to target cell. Old resources freed.");
}
