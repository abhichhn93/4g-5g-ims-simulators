#include "enb/enb_node.h"
#include "common/logger.h"
#include "common/pcap_writer.h"
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
    Logger::enb("thread started");
    try {
        setupServer();
        if (stop_.load()) return;
        std::thread rx_th([this]{ receiveLoop(); });
        commandLoop();
        rx_th.join();
    } catch (const std::exception& e) {
        Logger::warn("eNB", e.what());
    }
    Logger::enb("thread exiting");
}

void EnbNode::setupServer() {
    Logger::enb("TCP server on " + std::string(ENB_IP) + ":" + std::to_string(ENB_PORT));
    server_socket_ = Socket::createServer(ENB_IP, ENB_PORT);
    enb_ready_.store(true);
    Logger::enb("listening — waiting for MME...");
    while (!stop_.load()) {
        if (server_socket_.hasConnection(100)) {
            mme_conn_ = server_socket_.accept();
            Logger::enb("MME connected — S1 UP ✓");
            return;
        }
    }
}

void EnbNode::receiveLoop() {
    Logger::enb("[rx_th] started — handles DL NAS + ICSR from MME");
    while (!stop_.load()) {
        if (!mme_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!mme_conn_.recvFrame(payload)) break;
        if (payload.size() < 8) continue;
        MessageReader r(payload);
        switch (r.msgType()) {
            case MessageType::S1AP_DL_NAS_TRANSPORT:        handleDLNas(payload);  break;
            case MessageType::S1AP_INITIAL_CONTEXT_SETUP_REQ: handleICSR(payload); break;
            default: Logger::warn("eNB","[rx_th] unknown: "+std::string(msg_type_str(r.msgType())));
        }
    }
    Logger::enb("[rx_th] stopped");
}

void EnbNode::handleDLNas(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0; uint8_t nas_type=0; (void)nas_type;
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

    Logger::enb("[rx_th] ← DL NAS Transport  nas_type=0x" + [&]{ char b[8]; std::snprintf(b,8,"%02X",nas_type); return std::string(b); }());
    // PCAP: S1AP DL NAS Transport (MME→eNB)
    PcapWriter::instance().writeS1AP(
        nas_type == 0x52 ? "NAS-AuthRequest(DL)" :
        nas_type == 0x5D ? "NAS-SecurityModeCmd(DL)" : "NAS-DL",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP);

    if (nas_type == 0x5D) {
        // Security Mode Command received — UE activates algorithms, sends Complete
        Logger::enb("[rx_th] ← NAS Security Mode Command (0x5D)");
        Logger::ie_field("  Cipher: EEA2 (AES-128-CTR), Integrity: EIA2 (AES-128-CMAC)");
        Logger::ie_field("  UE derives KNASenc + KNASint from KASME");
        Logger::ie_field("  REAL: all subsequent NAS messages are integrity-protected + ciphered");
        Logger::enb("[rx_th] → NAS Security Mode Complete (0x5E)  UE activated security");

        std::lock_guard<std::mutex> lk(mme_send_mtx_);
        MessageWriter smc_complete(MessageType::S1AP_UL_NAS_TRANSPORT, next_seq_++);
        smc_complete.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
        smc_complete.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
        smc_complete.writeU8 (Tag::NAS_MSG_TYPE,   0x5E);  // Security Mode Complete
        mme_conn_.sendFrame(smc_complete.frame());
        PcapWriter::instance().writeS1AP("NAS-SecurityModeComplete(UL)",
            PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
            PcapWriter::IP_MME, PcapWriter::PORT_S1AP);
        return;
    }

    if (nas_type == 0x52 && rand_b.size() >= 8) {
        uint8_t res[8]; for(int i=0;i<8;i++) res[i]=rand_b[i]^0x55;
        Logger::enb("[rx_th] SIM: UE computes RES=RAND^0x55 — sending Auth Response");
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
            PcapWriter::IP_MME, PcapWriter::PORT_S1AP);
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

    Logger::enb("[rx_th] ← S1AP InitialContextSetupRequest [TS 36.413 §9.1.4.1]");
    Logger::ie_field("  MME-id=" + std::to_string(mme_id) + "  S-GW S1U-TEID=" + std::to_string(sgw_teid));
    Logger::ie_field("  NAS: Attach Accept (0x42)  UE-IP=" + std::string(ue_ip));
    Logger::enb("[rx_th] REAL: eNB sets up DRB (Data Radio Bearer) for UE");
    Logger::enb("[rx_th] REAL: eNB creates GTP-U tunnel to S-GW using S-GW's S1U TEID");
    Logger::enb("[rx_th] SIM: allocating eNB's S1-U TEID for the uplink tunnel");

    uint32_t enb_teid = next_enb_teid_.fetch_add(1);

    // PCAP: S1AP Initial Context Setup Request (MME→eNB)
    PcapWriter::instance().writeS1AP("S1AP-InitialContextSetupReq",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP);
    Logger::enb("[rx_th] → S1AP InitialContextSetupResponse  eNB S1-U TEID=" + std::to_string(enb_teid));
    { std::lock_guard<std::mutex> lk(mme_send_mtx_);
      MessageWriter rsp(MessageType::S1AP_INITIAL_CONTEXT_SETUP_RSP, next_seq_++);
      rsp.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
      rsp.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
      rsp.writeU32(Tag::ICSR_ENB_TEID,  enb_teid);
      mme_conn_.sendFrame(rsp.frame());
      PcapWriter::instance().writeS1AP("S1AP-InitialContextSetupRsp",
          PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
          PcapWriter::IP_MME, PcapWriter::PORT_S1AP); }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    Logger::enb("[rx_th] SIM: UE sends NAS Attach Complete (0x46)");
    { std::lock_guard<std::mutex> lk(mme_send_mtx_);
      MessageWriter ac(MessageType::S1AP_UL_NAS_TRANSPORT, next_seq_++);
      ac.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
      ac.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
      ac.writeU8 (Tag::NAS_MSG_TYPE,   0x46);
      mme_conn_.sendFrame(ac.frame());
      PcapWriter::instance().writeS1AP("NAS-AttachComplete(UL)",
          PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
          PcapWriter::IP_MME, PcapWriter::PORT_S1AP); }
}

void EnbNode::commandLoop() {
    Logger::enb("[enb_th] command loop started");
    while (!stop_.load()) {
        std::unique_lock<std::mutex> lk(cmd_mutex_);
        cmd_cv_.wait(lk,[this]{ return !cmd_queue_.empty()||stop_.load(); });
        while (!cmd_queue_.empty()) {
            std::string cmd=cmd_queue_.front(); cmd_queue_.pop();
            lk.unlock(); processCommand(cmd); lk.lock();
        }
    }
    Logger::enb("[enb_th] command loop stopped");
}

void EnbNode::processCommand(const std::string& cmd) {
    if (cmd.size()>=2 && cmd.substr(0,2)=="CR") {
        int n=1; try{if(cmd.size()>3) n=std::stoi(cmd.substr(3));}catch(...){}
        for(int i=0;i<n&&!stop_.load();++i) sendInitialUEMessage(next_enb_ue_id_++);
    }
}

void EnbNode::sendInitialUEMessage(uint32_t ue_index) {
    if (!mme_conn_.valid()) return;
    uint64_t imsi = BASE_IMSI + ue_index;
    Logger::enb("[enb_th] → InitialUEMessage  eNB-id=" + std::to_string(ue_index) + " IMSI=" + std::to_string(imsi));
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
    // PCAP: S1AP Initial UE Message = Attach Request
    PcapWriter::instance().writeS1AP("S1AP-InitialUEMsg(AttachReq)",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP);
}

void EnbNode::submitCommand(const std::string& cmd) {
    { std::lock_guard<std::mutex> lk(cmd_mutex_); cmd_queue_.push(cmd); }
    cmd_cv_.notify_one();
}
void EnbNode::requestStop() { cmd_cv_.notify_all(); }
