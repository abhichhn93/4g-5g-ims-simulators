#include "mme/mme_node.h"
#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/pcap_writer.h"
#include "common/subscriber_profile.h"
#include "common/nas_eps.h"
#include "common/s1ap_codec.h"
#include <chrono>
#include <thread>
#include <cstdio>
#include <stdexcept>
#include <cstring>

static constexpr const char* ENB_IP   = "127.0.0.1";
static constexpr uint16_t    ENB_PORT  = 38412;
static constexpr const char* HSS_IP   = "127.0.0.1";
static constexpr uint16_t    HSS_PORT  = 3868;
static constexpr const char* SGW_IP   = "127.0.0.1";
static constexpr uint16_t    SGW_PORT  = 2123;
static constexpr uint16_t    MME_S11_PORT = 2125;  // MME's own S11 UDP port

MmeNode::MmeNode(std::atomic<bool>& stop, std::atomic<bool>& enb_ready,
                 std::atomic<bool>& hss_ready, std::atomic<bool>& sgw_ready,
                 std::shared_ptr<Metrics> metrics)
    : stop_(stop), enb_ready_(enb_ready), hss_ready_(hss_ready), sgw_ready_(sgw_ready),
      metrics_(std::move(metrics))
{}

void MmeNode::run() {
    Logger::mme(Logger::Level::ENGINEER, "thread started");
    Logger::mme(Logger::Level::ENGINEER, "Phase 3: sharded UE store (64 buckets × shared_mutex) + GTP-C to S-GW");
    try {
        connectToNodes();
        if (stop_.load()) return;
        std::thread hss_rx([this]{ hssReceiveLoop(); });
        enbReceiveLoop();
        hss_rx.join();
    } catch (const std::exception& e) {
        Logger::warn("MME", e.what());
    }
    Logger::mme(Logger::Level::ENGINEER, "thread exiting");
}

void MmeNode::connectToNodes() {
    // HSS
    Logger::mme(Logger::Level::ENGINEER, "waiting for HSS...");
    while (!hss_ready_.load() && !stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop_.load()) return;
    hss_conn_ = Socket::connectTo(HSS_IP, HSS_PORT);
    Logger::mme(Logger::Level::ENGINEER, "HSS Diameter link UP ✓");

    // S-GW
    Logger::mme(Logger::Level::ENGINEER, "waiting for S-GW...");
    while (!sgw_ready_.load() && !stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop_.load()) return;
    sgw_udp_ = UdpSocket::bind("127.0.0.1", MME_S11_PORT);  // MME binds its S11 UDP port
    Logger::mme(Logger::Level::ENGINEER, "S11 UDP socket bound on port " + std::to_string(MME_S11_PORT));
    Logger::mme(Logger::Level::ENGINEER, "REAL: S11 is the interface between MME and S-GW (TS 23.401 §4.4.3.2)");

    // eNB
    Logger::mme(Logger::Level::ENGINEER, "waiting for eNB...");
    while (!enb_ready_.load() && !stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop_.load()) return;
    enb_conn_ = Socket::connectTo(ENB_IP, ENB_PORT);
    Logger::mme(Logger::Level::ENGINEER, "eNB S1 link UP ✓");
    Logger::mme(Logger::Level::ENGINEER, "All interfaces UP — MME ready");
}

// ─────────────────────────────────────────────────────────────
// hssReceiveLoop (hss_rx_th): PRODUCER for pending_auth_
// ─────────────────────────────────────────────────────────────
void MmeNode::hssReceiveLoop() {
    Logger::mme(Logger::Level::ENGINEER, "[hss_rx] loop started");
    while (!stop_.load()) {
        if (!hss_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!hss_conn_.recvFrame(payload)) break;
        if (payload.size() < 8) continue;
        MessageReader r(payload);
        if (r.msgType() != MessageType::DIA_AIA) continue;

        AuthVectors av{};
        uint64_t imsi = 0;
        while (r.hasMore()) {
            Tag tag; uint16_t len;
            if (!r.peek(tag, len)) break;
            switch (tag) {
                case Tag::DIA_IMSI: imsi = r.readU64(); break;
                case Tag::DIA_RAND: { auto b=r.readBytes(); if(b.size()==16) std::memcpy(av.rand,b.data(),16); break; }
                case Tag::DIA_AUTN: { auto b=r.readBytes(); if(b.size()==16) std::memcpy(av.autn,b.data(),16); break; }
                case Tag::DIA_XRES: { auto b=r.readBytes(); if(b.size()==8)  std::memcpy(av.xres,b.data(),8);  break; }
                case Tag::DIA_KASME:{ auto b=r.readBytes(); if(b.size()==32) std::memcpy(av.kasme,b.data(),32);break; }
                default: r.skip(); break;
            }
        }
        Logger::mme(Logger::Level::ENGINEER, "[hss_rx] ← Diameter AIA received for IMSI=" + std::to_string(imsi));
        { std::lock_guard<std::mutex> lk(pending_auth_mutex_); pending_auth_[imsi] = av; }
        pending_auth_cv_.notify_all();
    }
    Logger::mme(Logger::Level::ENGINEER, "[hss_rx] loop stopped");
}

// ─────────────────────────────────────────────────────────────
// enbReceiveLoop (mme_th): dispatch all eNB messages
// ─────────────────────────────────────────────────────────────
void MmeNode::enbReceiveLoop() {
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] eNB receive loop started");
    while (!stop_.load()) {
        if (!enb_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!enb_conn_.recvFrame(payload)) break;
        if (payload.size() < 8) continue;
        MessageReader r(payload);
        switch (r.msgType()) {
            case MessageType::S1AP_INITIAL_UE_MSG:           handleInitialUEMsg(payload);       break;
            case MessageType::S1AP_UL_NAS_TRANSPORT:         handleULNasTransport(payload);     break;
            case MessageType::S1AP_INITIAL_CONTEXT_SETUP_RSP: handleICSetupResponse(payload);   break;
            case MessageType::S1AP_TAU_REQUEST:              handleTauRequest(payload);         break;
            case MessageType::S1AP_HANDOVER_REQUIRED:        handleHandoverRequired(payload);   break;
            case MessageType::S1AP_HANDOVER_REQUEST_ACK:     handleHandoverRequestAck(payload); break;
            case MessageType::S1AP_ENB_STATUS_TRANSFER:      handleEnbStatusTransfer(payload);  break;
            case MessageType::S1AP_HANDOVER_NOTIFY:          handleHandoverNotify(payload);     break;
            case MessageType::S1AP_UE_CONTEXT_RELEASE_CMPL: handleUeContextRelCmpl(payload);   break;
            default: Logger::warn("MME","[mme_th] unexpected: "+std::string(msg_type_str(r.msgType())));
        }
    }
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] eNB receive loop stopped");
}

void MmeNode::handleInitialUEMsg(const std::vector<uint8_t>& payload) {
    uint32_t enb_id=0; uint64_t imsi=0;
    uint16_t mcc=0, mnc=0, tac=0; uint32_t cgi=0;
    (void)cgi;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if(!r.peek(tag,len)) break;
        switch(tag) {
            case Tag::ENB_UE_S1AP_ID: enb_id=r.readU32(); break;
            case Tag::TAI_MCC: mcc=r.readU16(); break;
            case Tag::TAI_MNC: mnc=r.readU16(); break;
            case Tag::TAI_TAC: tac=r.readU16(); break;
            case Tag::NAS_IMSI: imsi=r.readU64(); break;
            case Tag::NAS_KSI: { uint8_t k=r.readU8(); Logger::ie_field("  NAS: KSI="+std::to_string(k)+(k==7?" (full AKA needed)":"")); break; }
            default: r.skip(); break;
        }
    }

    VLog::step(1, 9, "ATTACH REQUEST",
               "UE", Logger::CLR_ENB, "eNB → MME", Logger::CLR_MME)
        .ie("IMSI",  std::to_string(imsi))
        .ie("TAI",   "MCC=" + std::to_string(mcc) + " MNC=" + std::to_string(mnc) + " TAC=0x" + std::to_string(tac))
        .ie("eNB-UE-S1AP-ID", std::to_string(enb_id))
        .state("UE", "DEREGISTERED → REG_PENDING")
        .next("MME creates UE context, sends Authentication-Information-Req to HSS")
        .flush();
    // PCAP: write this as seen from eNB side — no Diameter yet, just log marker

    // Create UE context in sharded store
    auto ctx = std::make_shared<UeContext>();
    ctx->imsi           = imsi;
    ctx->enb_ue_s1ap_id = enb_id;
    ctx->emm_state      = EmmState::REGISTERED_INITIATED;
    ctx->tai_mcc=mcc; ctx->tai_mnc=mnc; ctx->tai_tac=tac;
    ctx->attach_start   = std::chrono::steady_clock::now();  // Phase 4: latency start

    // Phase 4: Flyweight — get shared SubscriberProfile (same object for all "internet" UEs)
    auto profile = ProfileRegistry::instance().get("internet");
    ctx->profile = profile;  // shared_ptr — not a copy, just a reference
    Logger::ie_field("  Flyweight profile assigned: APN=" + profile->apn +
                     " QCI=" + std::to_string(profile->qci) +
                     " MaxDL=" + std::to_string(profile->max_dl_bps/1000000) + "Mbps");

    uint32_t mme_id = ue_store_.insert(ctx);

    Logger::mme(Logger::Level::ENGINEER, "[mme_th] UE context created in shard[" + std::to_string(mme_id%64) +
                "]  MME-id=" + std::to_string(mme_id));
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] SHARDING: mme_id%64=" + std::to_string(mme_id%64) +
                " → only locks 1 of 64 buckets → 64x less contention than single global mutex");

    // ── STEP 2: Diameter AIR to HSS ──────────────────────────
    VLog::step(2, 9, "AUTHENTICATION-INFORMATION-REQ (AIR)",
               "MME", Logger::CLR_MME, "HSS", Logger::CLR_HSS)
        .ie("Interface",  "Diameter S6a [TS 29.272 §7.2.5]")
        .ie("IMSI",       std::to_string(imsi))
        .ie("PLMN",       "MCC=" + std::to_string(mcc) + " MNC=" + std::to_string(mnc))
        .ie("Req-Vectors","1 (number of auth vectors requested)")
        .state("MME", "IDLE → AUTH_PENDING")
        .next("HSS runs Milenage algo: Ki+RAND → XRES, AUTN, KASME, CK, IK")
        .flush();

    { MessageWriter air(MessageType::DIA_AIR, next_seq_++);
      air.writeU64(Tag::DIA_IMSI, imsi);
      air.writeU32(Tag::DIA_PLMN, (uint32_t(mcc)<<16)|mnc);
      hss_conn_.sendFrame(air.frame());
      // PCAP: real Diameter AIR header → Wireshark shows "Diameter"
      PcapWriter::instance().writeDiameter(
          DiameterCmd::AUTH_INFO, DiameterApp::S6A, true,
          PcapWriter::IP_MME, 3868, PcapWriter::IP_HSS, 3868); }

    // BLOCK on cv.wait (hss_rx_th is PRODUCER, this thread is CONSUMER)
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] cv.wait — blocking until HSS AIA arrives [condition_variable]");
    AuthVectors av{};
    { std::unique_lock<std::mutex> lk(pending_auth_mutex_);
      pending_auth_cv_.wait(lk, [&]{ return pending_auth_.count(imsi)>0 || stop_.load(); });
      if (stop_.load()) return;
      av = pending_auth_[imsi]; pending_auth_.erase(imsi); }

    { auto c = ue_store_.find(mme_id); if(c){ c->auth = av; c->emm_state=EmmState::AUTH_PENDING; } }

    // ── STEP 3: AIA received ──────────────────────────────────
    char rand_hex[33]={}, xres_hex[17]={}, autn_hex[33]={};
    for(int i=0;i<16;i++) snprintf(rand_hex+i*2,3,"%02X",av.rand[i]);
    for(int i=0;i< 8;i++) snprintf(xres_hex+i*2,3,"%02X",av.xres[i]);
    for(int i=0;i<16;i++) snprintf(autn_hex+i*2,3,"%02X",av.autn[i]);

    VLog::step(3, 9, "AUTHENTICATION-INFORMATION-ANS (AIA)",
               "HSS", Logger::CLR_HSS, "MME", Logger::CLR_MME)
        .ie("RAND",  std::string(rand_hex) + "  (random challenge)")
        .ie("XRES",  std::string(xres_hex) + "  (expected response — MME stores this)")
        .ie("AUTN",  std::string(autn_hex) + "  (network auth token — UE verifies this)")
        .ie("KASME", "derived from CK+IK — root key for NAS/AS security")
        .state("MME", "AUTH_PENDING → CHALLENGE_SENT")
        .next("MME sends RAND+AUTN to UE via eNB (NAS Auth Request)")
        .flush();
    PcapWriter::instance().writeDiameter(
        DiameterCmd::AUTH_INFO, DiameterApp::S6A, false,
        PcapWriter::IP_HSS, 3868, PcapWriter::IP_MME, 3868);

    // Send Auth Request to eNB (DL NAS Transport)
    // ── STEP 4: NAS Auth Request ──────────────────────────────
    VLog::step(4, 9, "NAS AUTH REQUEST  (DL NAS Transport)",
               "MME", Logger::CLR_MME, "eNB → UE", Logger::CLR_ENB)
        .ie("RAND", std::string(rand_hex) + "  (UE uses RAND+Ki to compute RES)")
        .ie("AUTN", std::string(autn_hex) + "  (UE verifies network is authentic)")
        .ie("KSI",  "0 (Key Set Identifier)")
        .state("UE", "REG_PENDING → AUTH_CHALLENGE")
        .next("UE runs Milenage: verifies AUTN, computes RES, sends Auth Response");
    MessageWriter dl(MessageType::S1AP_DL_NAS_TRANSPORT, next_seq_++);
    dl.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    dl.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    dl.writeU8 (Tag::NAS_MSG_TYPE,   0x52);  // Auth Request
    dl.writeBytes(Tag::NAS_RAND, av.rand, 16);
    dl.writeBytes(Tag::NAS_AUTN, av.autn, 16);
    enb_conn_.sendFrame(dl.frame());
}

void MmeNode::handleULNasTransport(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_id=0; uint8_t nas_type=0; std::vector<uint8_t> res;

    MessageReader r(payload);
    while(r.hasMore()) {
        Tag tag; uint16_t len; if(!r.peek(tag,len)) break;
        switch(tag) {
            case Tag::MME_UE_S1AP_ID: mme_id=r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id=r.readU32(); break;
            case Tag::NAS_MSG_TYPE:   nas_type=r.readU8(); break;
            case Tag::NAS_RES:        res=r.readBytes(); break;
            default: r.skip(); break;
        }
    }

    if (nas_type == 0x53) {
        // Auth Response
        Logger::mme(Logger::Level::ENGINEER, "[mme_th] ← UL NAS Transport (Auth Response 0x53)  mme_id=" + std::to_string(mme_id));
        auto ctx = ue_store_.find(mme_id);
        if (!ctx) { Logger::warn("MME","no UE context for id="+std::to_string(mme_id)); return; }
        bool ok = (res.size()>=8 && std::memcmp(res.data(), ctx->auth.xres, 8)==0);
        if (ok) {
            char res_hex[17]={};
            for(int i=0;i<8&&i<(int)res.size();i++) snprintf(res_hex+i*2,3,"%02X",res[i]);
            VLog::step(5, 9, "NAS AUTH RESPONSE  (UL NAS Transport)",
                       "UE", Logger::CLR_ENB, "eNB → MME", Logger::CLR_MME)
                .ie("RES",  std::string(res_hex) + "  (UE computed this from RAND+Ki)")
                .ie("XRES", std::string(res_hex) + "  (matches — SIM card is genuine)")
                .ie("Result","RES == XRES ✓  Authentication SUCCESS")
                .state("UE",  "AUTH_CHALLENGE → AUTHENTICATED")
                .state("MME", "CHALLENGE_SENT → SESSION_PENDING")
                .next("MME sends Security Mode Command, then Create Session to S-GW")
                .flush();
            // ── STEP 6: NAS Security Mode Command ────────────
            VLog::step(6, 9, "NAS SECURITY MODE COMMAND  (DL NAS Transport)",
                       "MME", Logger::CLR_MME, "eNB → UE", Logger::CLR_ENB)
                .ie("NAS Msg Type",  "0x5D = Security Mode Command")
                .ie("NAS Cipher",    "EEA2 (AES-128-CTR) — NAS ciphering algorithm")
                .ie("NAS Integrity", "EIA2 (AES-128-CMAC) — NAS integrity algorithm")
                .ie("KASME",         "root key derived from CK+IK (from AIA)")
                .ie("KNASenc",       "derived from KASME — encrypts NAS messages")
                .ie("KNASint",       "derived from KASME — MAC-I integrity tag")
                .state("UE",  "AUTHENTICATED → SECURITY_MODE")
                .state("MME", "AUTH_PENDING → SECURITY_MODE")
                .next("UE activates selected algorithms, sends Security Mode Complete")
                .flush();

            // PCAP write happens at the eNB's handleDLNas when it receives this
            // (nas_type==0x5D), consistent with the Auth Request DL message above.
            { MessageWriter smc(MessageType::S1AP_DL_NAS_TRANSPORT, next_seq_++);
              smc.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
              smc.writeU32(Tag::ENB_UE_S1AP_ID, ctx->enb_ue_s1ap_id);
              smc.writeU8 (Tag::NAS_MSG_TYPE,   0x5D);  // Security Mode Command
              enb_conn_.sendFrame(smc.frame()); }

            ctx->emm_state = EmmState::SESSION_PENDING;
            handleAuthSuccess(mme_id);
        } else {
            Logger::warn("MME","AUTH FAILED for IMSI="+std::to_string(ctx->imsi));
        }
    } else if (nas_type == 0x5E) {
        // Security Mode Complete — NAS security activated
        Logger::mme(Logger::Level::ENGINEER, "[mme_th] ← NAS Security Mode Complete (0x5E)  mme_id=" + std::to_string(mme_id));
        Logger::ie_field("  EEA2 + EIA2 activated ✓ — all NAS now encrypted + integrity-protected");
        Logger::ie_field("  INTERVIEW: this is NAS-layer security (different from AS/radio security)");
        Logger::ie_field("  AS security activated later via KeNB in InitialContextSetupRequest");
        PcapWriter::instance().writeS1AP("NAS-SecurityModeComplete(UL-seen-by-MME)",
            PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
            PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
            s1ap::buildUlNasTransport(mme_id, enb_id, nas_eps::SECURITY_MODE_COMPLETE));
    } else if (nas_type == 0x46) {
        // Attach Complete
        Logger::mme(Logger::Level::ENGINEER, "[mme_th] ← UL NAS Transport (Attach Complete 0x46)  mme_id=" + std::to_string(mme_id));
        auto ctx = ue_store_.find(mme_id);
        if (!ctx) return;
        ctx->emm_state = EmmState::REGISTERED;
        auto* b = ctx->defaultBearer();

        auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ctx->attach_start).count();
        if (metrics_) metrics_->recordAttach(double(latency_ms));

        VLog::step(8, 9, "ATTACH COMPLETE  ✓  UE REGISTERED",
                   "UE", Logger::CLR_ENB, "eNB → MME", Logger::CLR_MME)
            .ie("IMSI",    std::to_string(ctx->imsi))
            .ie("UE IP",   (b ? b->ue_ip : "?") + "  (allocated by P-GW)")
            .ie("QCI-9",   "Default bearer ACTIVE — internet/SIP signalling")
            .ie("Latency", std::to_string(latency_ms) + "ms  (full attach end-to-end)")
            .state("UE",   "AUTHENTICATED → REGISTERED")
            .state("MME",  "SESSION_PENDING → REGISTERED")
            .ie("VoLTE",   "Run mme_ims to add IMS registration + QCI=1 voice bearer")
            .next("UE has LTE data. Type REGISTER in mme_ims for VoLTE on top of this bearer")
            .flush();
    }
}

void MmeNode::handleAuthSuccess(uint32_t mme_id) {
    auto ctx = ue_store_.find(mme_id);
    if (!ctx) return;
    VLog::step(7, 9, "CREATE SESSION REQUEST  (GTPv2)",
               "MME", Logger::CLR_MME, "S-GW → P-GW", Logger::CLR_SGW)
        .ie("Interface", "GTP-Cv2 S11 [TS 29.274] port 2123")
        .ie("IMSI",      std::to_string(ue_store_.find(mme_id) ? ue_store_.find(mme_id)->imsi : 0))
        .ie("APN",       "internet  (Access Point Name)")
        .ie("PDN-Type",  "IPv4")
        .ie("QCI",       "9 (default bearer — internet traffic)")
        .state("Bearer", "NONE → CREATING")
        .next("S-GW forwards to P-GW, P-GW queries PCRF for QCI policy via Gx")
        .flush();
    PcapWriter::instance().writeGTPv2(GtpMsgType::CREATE_SESSION_REQ, 0,
        PcapWriter::IP_MME, 2123, PcapWriter::IP_SGW, 2123);
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] → Starting GTP-C session setup (Phase 3)");
    if (!sendCreateSession(mme_id, ctx->imsi)) return;

    // Send Initial Context Setup Request to eNB
    auto* bearer = ctx->defaultBearer();
    if (!bearer) { Logger::warn("MME","no bearer after session setup"); return; }

    Logger::mme(Logger::Level::ENGINEER, "[mme_th] → S1AP InitialContextSetupRequest [TS 36.413 §9.1.4.1]");
    Logger::ie_field("  S-GW S1-U TEID=" + std::to_string(bearer->sgw_s1u_teid) +
                     "  UE IP=" + bearer->ue_ip);
    Logger::ie_field("  NAS: Attach Accept + Activate Default EPS Bearer Context Request");
    Logger::ie_field("  REAL: also carries KeNB (eNB security key derived from Kasme)");

    uint8_t sgw_ip[4] = {127,0,0,1};
    MessageWriter icsr(MessageType::S1AP_INITIAL_CONTEXT_SETUP_REQ, next_seq_++);
    icsr.writeU32  (Tag::MME_UE_S1AP_ID, mme_id);
    icsr.writeU32  (Tag::ENB_UE_S1AP_ID, ctx->enb_ue_s1ap_id);
    icsr.writeU32  (Tag::ICSR_AMBR_UL,   50000000);
    icsr.writeU32  (Tag::ICSR_AMBR_DL,   100000000);
    icsr.writeBytes(Tag::ICSR_SGW_S1U_IP, sgw_ip, 4);
    icsr.writeU32  (Tag::ICSR_SGW_TEID,  bearer->sgw_s1u_teid);
    icsr.writeU8   (Tag::NAS_EBI,        bearer->ebi);
    // Embed NAS Attach Accept fields
    icsr.writeU8   (Tag::NAS_MSG_TYPE,   0x42);  // Attach Accept
    { uint8_t ip_b[4];
      unsigned a,b2,c,d;
      if (std::sscanf(bearer->ue_ip.c_str(),"%u.%u.%u.%u",&a,&b2,&c,&d)==4) {
          ip_b[0]=a; ip_b[1]=b2; ip_b[2]=c; ip_b[3]=d;
          icsr.writeBytes(Tag::NAS_UE_IP, ip_b, 4);
      }
    }
    enb_conn_.sendFrame(icsr.frame());
}

bool MmeNode::sendCreateSession(uint32_t mme_id, uint64_t imsi) {
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] → GTP-C CreateSessionReq to S-GW [TS 29.274 §7.2.1]");
    Logger::ie_field("  IMSI=" + std::to_string(imsi) + "  APN=internet  EBI=5  QCI=9");

    MessageWriter req(MessageType::GTP_CREATE_SESSION_REQ, next_seq_++);
    req.writeU64(Tag::GTP_IMSI,    imsi);
    req.writeStr(Tag::GTP_APN,     "internet");
    req.writeU8 (Tag::GTP_EBI,     5);
    req.writeU8 (Tag::GTP_QCI,     9);
    req.writeU32(Tag::GTP_AMBR_UL, 50000000);
    req.writeU32(Tag::GTP_AMBR_DL, 100000000);
    sgw_udp_.sendTo(req.udpPayload(), SGW_IP, SGW_PORT);

    // Blocking wait for S-GW response (synchronous for Phase 3 single-UE flow)
    // PHASE 4 TODO: make async with condition_variable like HSS pattern
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] waiting for S-GW CreateSessionRsp (blocking UDP recv, 5s timeout)...");
    std::vector<uint8_t> resp; sockaddr_in sgw_addr{};
    if (!sgw_udp_.recvWithTimeout(resp, sgw_addr, 5000)) {
        Logger::warn("MME","S-GW CreateSessionRsp timeout"); return false;
    }

    uint8_t cause=0; uint32_t sgw_s11=0, sgw_s1u=0; uint64_t imsi2=0;
    std::vector<uint8_t> ue_ip_bytes;

    MessageReader r(resp);
    while(r.hasMore()) {
        Tag tag; uint16_t len; if(!r.peek(tag,len)) break;
        switch(tag) {
            case Tag::GTP_CAUSE:       cause      = r.readU8();    break;
            case Tag::GTP_SENDER_TEID: sgw_s11    = r.readU32();   break;
            case Tag::GTP_BEARER_TEID: sgw_s1u    = r.readU32();   break;
            case Tag::GTP_IMSI:        imsi2      = r.readU64();   break;
            case Tag::GTP_UE_IP:       ue_ip_bytes= r.readBytes(); break;
            default: r.skip(); break;
        }
    }
    (void)imsi2;

    if (cause != 16 || ue_ip_bytes.size() < 4) {
        Logger::warn("MME","S-GW CreateSessionRsp failed cause="+std::to_string(cause)); return false;
    }

    char ue_ip[32]; std::snprintf(ue_ip,32,"%d.%d.%d.%d",ue_ip_bytes[0],ue_ip_bytes[1],ue_ip_bytes[2],ue_ip_bytes[3]);
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] ← S-GW CreateSessionRsp — UE IP=" + std::string(ue_ip));

    // Store bearer in UE context
    Bearer b{};
    b.ebi=5; b.qci=9; b.sgw_s11_teid=sgw_s11; b.sgw_s1u_teid=sgw_s1u; b.ue_ip=ue_ip;
    auto ctx = ue_store_.find(mme_id);
    if (ctx) ctx->bearers.push_back(b);
    return true;
}

void MmeNode::handleICSetupResponse(const std::vector<uint8_t>& payload) {
    uint32_t mme_id=0, enb_teid=0;
    (void)enb_teid;

    MessageReader r(payload);
    while(r.hasMore()) {
        Tag tag; uint16_t len; if(!r.peek(tag,len)) break;
        switch(tag) {
            case Tag::MME_UE_S1AP_ID: mme_id   = r.readU32(); break;
            case Tag::ICSR_ENB_TEID:  enb_teid = r.readU32(); break;
            default: r.skip(); break;
        }
    }

    Logger::mme(Logger::Level::ENGINEER, "[mme_th] ← S1AP InitialContextSetupResponse  eNB S1-U TEID=" + std::to_string(enb_teid));
    Logger::ie_field("  eNB allocated TEID=" + std::to_string(enb_teid) + " for GTP-U downlink tunnel");

    // Store eNB's TEID in UE context
    auto ctx = ue_store_.find(mme_id);
    if (ctx) { auto* b=ctx->defaultBearer(); if(b) b->enb_s1u_teid=enb_teid; }

    // Send Modify Bearer Request to S-GW (tell S-GW the eNB's S1-U TEID)
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] → GTP-C ModifyBearerReq to S-GW [TS 29.274 §7.2.7]");
    Logger::ie_field("  Telling S-GW: 'eNB's S1-U TEID=" + std::to_string(enb_teid) +
                     " — route downlink data there'");

    uint64_t imsi = ctx ? ctx->imsi : 0;
    MessageWriter req(MessageType::GTP_MODIFY_BEARER_REQ, next_seq_++);
    req.writeU32(Tag::GTP_ENB_TEID, enb_teid);
    req.writeU64(Tag::GTP_IMSI,     imsi);
    sgw_udp_.sendTo(req.udpPayload(), SGW_IP, SGW_PORT);

    // Wait for Modify Bearer Response
    std::vector<uint8_t> resp; sockaddr_in sgw_addr{};
    if (!sgw_udp_.recvWithTimeout(resp, sgw_addr, 3000)) {
        Logger::warn("MME","ModifyBearerRsp timeout"); return;
    }
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] ← S-GW ModifyBearerRsp ✓");
    Logger::mme(Logger::Level::ENGINEER, "[mme_th] Data path established: UE ←GTP-U→ eNB ←GTP-U→ S-GW ←GTP-U→ P-GW ←→ Internet");
    Logger::sys("INTERVIEW: 'The data tunnel is now live. User plane goes eNB→S-GW→P-GW→Internet.");
    Logger::sys("           Control plane (NAS) goes eNB→MME. Completely separate paths.'");
}

void MmeNode::printStatus() const {
    size_t total = ue_store_.size();
    Logger::sys("=== UE Context Table (" + std::to_string(total) + " UEs, 64 shards) ===");
    if (total == 0) { Logger::sys("  No UEs. Type 'CR 1'."); return; }
    ue_store_.forEach([](const UeContext& ctx) {
        const auto* b = ctx.defaultBearer();
        Logger::ie_field(
            "UE[mme=" + std::to_string(ctx.mme_ue_s1ap_id) + "]"
            "  IMSI=" + std::to_string(ctx.imsi) +
            "  State=" + emm_state_str(ctx.emm_state) +
            "  IP=" + (b ? b->ue_ip : "-"));
    });
}

// ═════════════════════════════════════════════════════════════
// TAU — Tracking Area Update (TS 24.301 §5.5.3)
// ═════════════════════════════════════════════════════════════
void MmeNode::handleTauRequest(const std::vector<uint8_t>& payload) {
    uint32_t mme_id = 0, enb_id = 0;
    uint64_t imsi   = 0;
    uint8_t  tau_type = 0;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id   = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id   = r.readU32(); break;
            case Tag::TAU_IMSI:       imsi      = r.readU64(); break;
            case Tag::TAU_UPDATE_TYPE:tau_type  = r.readU8();  break;
            default: r.skip(); break;
        }
    }

    // ── BEGINNER ──────────────────────────────────────────────
    VLog::step(1, 3, "TRACKING AREA UPDATE REQUEST",
               "UE", Logger::CLR_ENB, "eNB → MME", Logger::CLR_MME)
        .ie("NAS Type",   "0x48 = TAU Request [TS 24.301 §8.2.29]")
        .ie("IMSI",       std::to_string(imsi))
        .ie("Update Type",tau_type == 0 ? "TA updating" : "periodic TAU")
        .ie("Old TAI",    "MCC=404 MNC=10 TAC=1  (UE came from cell 1)")
        .state("UE", "REGISTERED → TAU_PENDING")
        .next("MME validates: is the new TAI in our served-TAI-list?")
        .flush();

    // ── ENGINEER ──────────────────────────────────────────────
    Logger::mme(Logger::Level::ENGINEER,
        "[TAU] Lookup UE context mme_id=" + std::to_string(mme_id));
    Logger::ie_field("  TS 23.401 §5.3.3: MME checks if new TAI is in its TA list.");
    Logger::ie_field("  If TAI not served → TAU Reject with cause #13 (roaming not allowed).");
    Logger::ie_field("  SIM: TAI accepted — same MME, just moved to adjacent cell (TAC=2).");

    // ── INTERVIEW ─────────────────────────────────────────────
    Logger::mme(Logger::Level::INTERVIEW_T,
        "[INTERVIEW] Q: When does a UE send TAU Request?");
    Logger::mme(Logger::Level::INTERVIEW_C,
        "A: When UE moves to a new Tracking Area that is NOT in its Tracking Area List.");
    Logger::mme(Logger::Level::INTERVIEW_C,
        "   The TAI list is given during Attach and refreshed in each TAU Accept.");
    Logger::mme(Logger::Level::INTERVIEW_C,
        "   Periodic TAU: UE sends TAU even without moving, just to keep context alive.");
    Logger::mme(Logger::Level::INTERVIEW_C,
        "   TS 23.401 §5.3.3.1: if MME changes, old MME context is transferred via S10.");

    // PCAP: TAU Request (UL NAS Transport eNB→MME)
    PcapWriter::instance().writeS1AP("NAS-TauRequest(UL)",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildUlNasTransport(mme_id, enb_id, nas_eps::buildTauRequest(imsi)));

    // ── STEP 2: Send TAU Accept ────────────────────────────────
    VLog::step(2, 3, "TRACKING AREA UPDATE ACCEPT",
               "MME", Logger::CLR_MME, "eNB → UE", Logger::CLR_ENB)
        .ie("NAS Type",  "0x49 = TAU Accept [TS 24.301 §8.2.28]")
        .ie("EPS Update Result", "TA updated (0x00)")
        .ie("T3412",     "54 minutes — periodic TAU timer reset")
        .ie("TAI List",  "MCC=404 MNC=10 TAC=2 — new served TA added to UE's list")
        .state("UE", "TAU_PENDING → REGISTERED")
        .next("UE sends TAU Complete (for GUTI reallocation, else flow ends here)")
        .flush();

    Logger::ie_field("  ENGINEER: No S-GW path update needed (same S-GW, intra-SGW TAU).");
    Logger::ie_field("  REAL: if S-GW changes → MME creates new session on new S-GW, deletes old.");
    Logger::ie_field("  INTERVIEW: TAU differs from Attach: no re-authentication if context valid.");

    MessageWriter tauacc(MessageType::S1AP_TAU_ACCEPT, next_seq_++);
    tauacc.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    tauacc.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    tauacc.writeU8 (Tag::TAU_UPDATE_TYPE, 0x00);  // TA updated
    enb_conn_.sendFrame(tauacc.frame());

    // PCAP: TAU Accept (DL NAS Transport MME→eNB)
    PcapWriter::instance().writeS1AP("NAS-TauAccept(DL)",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildDlNasTransport(mme_id, enb_id, nas_eps::buildTauAccept()));

    VLog::step(3, 3, "TAU COMPLETE  ✓",
               "MME", Logger::CLR_MME, nullptr, nullptr)
        .ie("Result", "UE successfully updated its Tracking Area")
        .ie("Context", "Bearer preserved, IP unchanged — seamless mobility")
        .state("UE", "REGISTERED (new TAI=2)")
        .flush();
}

// ═════════════════════════════════════════════════════════════
// S1 HANDOVER — TS 36.413 §8.4 (7-step flow)
// ═════════════════════════════════════════════════════════════
void MmeNode::handleHandoverRequired(const std::vector<uint8_t>& payload) {
    uint32_t mme_id = 0, enb_id = 0; uint64_t imsi = 0;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id = r.readU32(); break;
            case Tag::TAU_IMSI:       imsi   = r.readU64(); break;
            default: r.skip(); break;
        }
    }

    VLog::step(1, 7, "HANDOVER REQUIRED  [TS 36.413 §8.4]",
               "src-eNB", Logger::CLR_ENB, "MME", Logger::CLR_MME)
        .ie("S1AP Proc",  "HandoverPreparation — procedure code 0")
        .ie("IMSI",       std::to_string(imsi))
        .ie("HO Type",    "intralte (intra-frequency, same MME)")
        .ie("Cause",      "radioNetwork: handover-desirable-for-radio-reasons")
        .ie("Target",     "eNB cell TAC=2 (simulated adjacent cell)")
        .ie("Src Cntr",   "Source-to-Target Transparent Container (RRC config, opaque to MME)")
        .state("UE", "REGISTERED → HO_PREPARATION")
        .next("MME sends HandoverRequest to target eNB to reserve resources")
        .flush();

    Logger::mme(Logger::Level::ENGINEER,
        "[HO] Step 1: HandoverRequired received  mme_id=" + std::to_string(mme_id));
    Logger::ie_field("  TS 36.413 §8.4.1: MME selects target eNB from TargetID IE.");
    Logger::ie_field("  SIM: target eNB = same connection (intra-eNB, different cell).");

    Logger::mme(Logger::Level::INTERVIEW_T,
        "[INTERVIEW] Q: What triggers a Handover Required?");
    Logger::mme(Logger::Level::INTERVIEW_C,
        "A: RRM (Radio Resource Management) in the eNB. When a neighbour cell RSRP");
    Logger::mme(Logger::Level::INTERVIEW_C,
        "   exceeds the serving cell by A3-event threshold, eNB decides to hand off.");
    Logger::mme(Logger::Level::INTERVIEW_C,
        "   S1 HO is used when source and target eNBs do NOT have X2 link (or it fails).");

    PcapWriter::instance().writeS1AP("S1AP-HandoverRequired",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverRequired(mme_id, enb_id));

    // ── STEP 2: HandoverRequest to target eNB ─────────────────
    VLog::step(2, 7, "HANDOVER REQUEST  [TS 36.413 §8.4.2]",
               "MME", Logger::CLR_MME, "tgt-eNB", Logger::CLR_ENB)
        .ie("S1AP Proc", "HandoverResourceAllocation — procedure code 1")
        .ie("HO Type",   "intralte")
        .ie("SecurityCtx","KeNB* — new AS security key for target cell")
        .ie("E-RABs",    "EBI=5, QCI=9 — default bearer to setup at target")
        .ie("Tgt Cntr",  "Source-to-Target Transparent Container forwarded")
        .state("tgt-eNB", "IDLE → PREPARING_HO")
        .next("Target eNB allocates radio resources, sends Handover Request Ack")
        .flush();

    Logger::ie_field("  REAL: MME also derives KeNB* from current KeNB (AS key refresh).");
    Logger::ie_field("  REAL: Target eNB decodes the RRC reconfiguration from the Transparent Container.");

    MessageWriter hor(MessageType::S1AP_HANDOVER_REQUEST, next_seq_++);
    hor.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    hor.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    hor.writeU8 (Tag::HO_TYPE, 0);  // intralte
    enb_conn_.sendFrame(hor.frame());

    PcapWriter::instance().writeS1AP("S1AP-HandoverRequest",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverRequest(mme_id, enb_id));
}

void MmeNode::handleHandoverRequestAck(const std::vector<uint8_t>& payload) {
    uint32_t mme_id = 0, enb_id = 0, new_teid = 0;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id   = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id   = r.readU32(); break;
            case Tag::HO_ENB_TEID_NEW:new_teid  = r.readU32(); break;
            default: r.skip(); break;
        }
    }

    VLog::step(3, 7, "HANDOVER REQUEST ACK  [TS 36.413 §8.4.2]",
               "tgt-eNB", Logger::CLR_ENB, "MME", Logger::CLR_MME)
        .ie("S1AP Proc",    "HandoverResourceAllocation — successful outcome")
        .ie("new eNB TEID", std::to_string(new_teid) + " (target cell S1-U TEID)")
        .ie("Tgt Cntr",     "Target-to-Source Transparent Container (RRC reconfig for UE)")
        .state("tgt-eNB", "PREPARING_HO → READY")
        .next("MME sends HandoverCommand to source eNB — tells UE to move now")
        .flush();

    Logger::ie_field("  REAL: MME stores new target TEID for later Modify Bearer to S-GW.");
    Logger::ie_field("  REAL: Transparent Container carries RRCConnectionReconfiguration for UE.");

    PcapWriter::instance().writeS1AP("S1AP-HandoverRequestAck",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverRequestAck(mme_id, enb_id));

    // Store new eNB TEID in UE context for later path switch
    auto ctx = ue_store_.find(mme_id);
    if (ctx) { auto* b = ctx->defaultBearer(); if (b) b->enb_s1u_teid = new_teid; }

    // ── STEP 4: HandoverCommand to source eNB ─────────────────
    VLog::step(4, 7, "HANDOVER COMMAND  [TS 36.413 §8.4.1]",
               "MME", Logger::CLR_MME, "src-eNB", Logger::CLR_ENB)
        .ie("S1AP Proc", "HandoverPreparation — successful outcome")
        .ie("Contents",  "Target-to-Source Transparent Container (for UE)")
        .ie("Meaning",   "Source eNB: forward this to UE, UE will disconnect from you")
        .state("src-eNB", "SERVING → RELEASING")
        .state("UE",      "HO_PREPARATION → HO_EXECUTION")
        .next("UE connects to target cell, source eNB sends ENBStatusTransfer")
        .flush();

    Logger::ie_field("  INTERVIEW: After HandoverCommand, UE is in RRC HO Execution state.");
    Logger::ie_field("  REAL: UE reads RRCConnectionReconfiguration, sync to target cell, sends Complete.");

    MessageWriter hocmd(MessageType::S1AP_HANDOVER_COMMAND, next_seq_++);
    hocmd.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    hocmd.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    enb_conn_.sendFrame(hocmd.frame());

    PcapWriter::instance().writeS1AP("S1AP-HandoverCommand",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverCommand(mme_id, enb_id));
}

void MmeNode::handleEnbStatusTransfer(const std::vector<uint8_t>& payload) {
    uint32_t mme_id = 0, enb_id = 0, pdcp_ul = 0, pdcp_dl = 0;

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

    VLog::step(5, 7, "ENB STATUS TRANSFER  [TS 36.413 §8.4.3]",
               "src-eNB", Logger::CLR_ENB, "MME", Logger::CLR_MME)
        .ie("Proc",     "eNBStatusTransfer — procedure code 24")
        .ie("PDCP SN UL", std::to_string(pdcp_ul) + " (next expected uplink seq num)")
        .ie("PDCP SN DL", std::to_string(pdcp_dl) + " (downlink seq num for re-ordering)")
        .ie("Purpose",  "Allows lossless handover — target eNB reorders in-flight packets")
        .next("MME forwards PDCP status to target eNB (MMEStatusTransfer)")
        .flush();

    Logger::ie_field("  INTERVIEW: Why send PDCP SNs? In-flight packets may arrive out-of-order.");
    Logger::ie_field("  Target eNB uses SN to re-order SDUs before delivering to UE.");
    Logger::ie_field("  Without this: packet loss during HO → TCP stall → user sees stutter.");

    PcapWriter::instance().writeS1AP("S1AP-ENBStatusTransfer",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildENBStatusTransfer(mme_id, enb_id));

    // Forward to target eNB (same eNB in our sim)
    MessageWriter mst(MessageType::S1AP_MME_STATUS_TRANSFER, next_seq_++);
    mst.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    mst.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    mst.writeU32(Tag::HO_PDCP_SN_UL, pdcp_ul);
    mst.writeU32(Tag::HO_PDCP_SN_DL, pdcp_dl);
    enb_conn_.sendFrame(mst.frame());

    PcapWriter::instance().writeS1AP("S1AP-MMEStatusTransfer",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildMMEStatusTransfer(mme_id, enb_id));
}

void MmeNode::handleHandoverNotify(const std::vector<uint8_t>& payload) {
    uint32_t mme_id = 0, enb_id = 0, new_teid = 0;

    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id   = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id   = r.readU32(); break;
            case Tag::HO_ENB_TEID_NEW:new_teid  = r.readU32(); break;
            default: r.skip(); break;
        }
    }

    VLog::step(6, 7, "HANDOVER NOTIFY  [TS 36.413 §8.4.3]",
               "tgt-eNB", Logger::CLR_ENB, "MME", Logger::CLR_MME)
        .ie("Proc",    "HandoverNotification — procedure code 2")
        .ie("ECGI",    "E-CGI of target cell — UE is now HERE")
        .ie("TAI",     "New Tracking Area (TAC=2)")
        .state("UE", "HO_EXECUTION → REGISTERED (target cell)")
        .next("MME sends GTP Modify Bearer to S-GW (path switch to new eNB)")
        .flush();

    Logger::ie_field("  INTERVIEW: HandoverNotify is the 'UE landed' signal.");
    Logger::ie_field("  After this: MME MUST redirect user-plane to target eNB via Modify Bearer.");

    PcapWriter::instance().writeS1AP("S1AP-HandoverNotify",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildHandoverNotify(mme_id, enb_id));

    // Path switch: Modify Bearer to S-GW with new eNB's TEID
    auto ctx = ue_store_.find(mme_id);
    uint32_t final_teid = new_teid ? new_teid : (ctx && ctx->defaultBearer() ? ctx->defaultBearer()->enb_s1u_teid : 200);
    sendModifyBearer(mme_id, final_teid);

    // Release source eNB context
    MessageWriter relcmd(MessageType::S1AP_UE_CONTEXT_RELEASE_CMD, next_seq_++);
    relcmd.writeU32(Tag::MME_UE_S1AP_ID, mme_id);
    relcmd.writeU32(Tag::ENB_UE_S1AP_ID, enb_id);
    relcmd.writeU8 (Tag::HO_CAUSE, 0);  // handover-desirable
    enb_conn_.sendFrame(relcmd.frame());

    PcapWriter::instance().writeS1AP("S1AP-UEContextReleaseCmd",
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        s1ap::buildUEContextReleaseCommand(mme_id, enb_id));
}

void MmeNode::handleUeContextRelCmpl(const std::vector<uint8_t>& payload) {
    uint32_t mme_id = 0, enb_id = 0;
    MessageReader r(payload);
    while (r.hasMore()) {
        Tag tag; uint16_t len; if (!r.peek(tag, len)) break;
        switch (tag) {
            case Tag::MME_UE_S1AP_ID: mme_id = r.readU32(); break;
            case Tag::ENB_UE_S1AP_ID: enb_id = r.readU32(); break;
            default: r.skip(); break;
        }
    }

    VLog::step(7, 7, "UE CONTEXT RELEASE COMPLETE  ✓  HANDOVER DONE",
               "src-eNB", Logger::CLR_ENB, "MME", Logger::CLR_MME)
        .ie("Proc",   "UEContextRelease — procedure code 23")
        .ie("Result", "Source eNB released radio and S1 resources")
        .state("src-eNB", "RELEASING → IDLE")
        .state("UE",      "REGISTERED (target cell, seamless handover)")
        .flush();

    Logger::ie_field("  INTERVIEW: The full S1 HO takes 50-80ms in production.");
    Logger::ie_field("  X2 HO (when X2 link exists) is faster: 30-50ms, no MME involvement.");
    Logger::ie_field("  S1 HO is mandatory fallback: no X2, or inter-MME, or inter-SGW cases.");
    Logger::sys("HO COMPLETE: UE seamlessly moved from TAC=1 to TAC=2. Bearer preserved.");

    PcapWriter::instance().writeS1AP("S1AP-UEContextReleaseCmpl",
        PcapWriter::IP_ENB, PcapWriter::PORT_S1AP,
        PcapWriter::IP_MME, PcapWriter::PORT_S1AP,
        s1ap::buildUEContextReleaseComplete(mme_id, enb_id));
    (void)enb_id;
}

bool MmeNode::sendModifyBearer(uint32_t mme_id, uint32_t new_enb_teid) {
    auto ctx = ue_store_.find(mme_id);
    uint64_t imsi = ctx ? ctx->imsi : 0;

    Logger::mme(Logger::Level::ENGINEER,
        "[HO] Path switch: Modify Bearer → S-GW with new eNB TEID=" + std::to_string(new_enb_teid));
    Logger::ie_field("  REAL: S-GW re-routes downlink GTP-U tunnel to new eNB's S1-U TEID.");
    Logger::ie_field("  This is the 'path switch' — user plane now flows through target eNB.");

    PcapWriter::instance().writeGTPv2(GtpMsgType::MODIFY_BEARER_REQ, 0,
        PcapWriter::IP_MME, 2123, PcapWriter::IP_SGW, 2123);

    MessageWriter req(MessageType::GTP_MODIFY_BEARER_REQ, next_seq_++);
    req.writeU32(Tag::GTP_ENB_TEID, new_enb_teid);
    req.writeU64(Tag::GTP_IMSI,     imsi);
    sgw_udp_.sendTo(req.udpPayload(), SGW_IP, SGW_PORT);

    std::vector<uint8_t> resp; sockaddr_in sgw_addr{};
    if (!sgw_udp_.recvWithTimeout(resp, sgw_addr, 3000)) {
        Logger::warn("MME", "[HO] Modify Bearer timeout"); return false;
    }
    PcapWriter::instance().writeGTPv2(GtpMsgType::MODIFY_BEARER_RSP, 0,
        PcapWriter::IP_SGW, 2123, PcapWriter::IP_MME, 2123);
    Logger::mme(Logger::Level::ENGINEER, "[HO] Modify Bearer RSP ✓ — data path switched");
    return true;
}
