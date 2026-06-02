#include "mme/mme_node.h"
#include "common/logger.h"
#include "common/visual_logger.h"
#include "common/pcap_writer.h"
#include "common/subscriber_profile.h"
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
    Logger::mme("thread started");
    Logger::mme("Phase 3: sharded UE store (64 buckets × shared_mutex) + GTP-C to S-GW");
    try {
        connectToNodes();
        if (stop_.load()) return;
        std::thread hss_rx([this]{ hssReceiveLoop(); });
        enbReceiveLoop();
        hss_rx.join();
    } catch (const std::exception& e) {
        Logger::warn("MME", e.what());
    }
    Logger::mme("thread exiting");
}

void MmeNode::connectToNodes() {
    // HSS
    Logger::mme("waiting for HSS...");
    while (!hss_ready_.load() && !stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop_.load()) return;
    hss_conn_ = Socket::connectTo(HSS_IP, HSS_PORT);
    Logger::mme("HSS Diameter link UP ✓");

    // S-GW
    Logger::mme("waiting for S-GW...");
    while (!sgw_ready_.load() && !stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop_.load()) return;
    sgw_udp_ = UdpSocket::bind("127.0.0.1", MME_S11_PORT);  // MME binds its S11 UDP port
    Logger::mme("S11 UDP socket bound on port " + std::to_string(MME_S11_PORT));
    Logger::mme("REAL: S11 is the interface between MME and S-GW (TS 23.401 §4.4.3.2)");

    // eNB
    Logger::mme("waiting for eNB...");
    while (!enb_ready_.load() && !stop_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (stop_.load()) return;
    enb_conn_ = Socket::connectTo(ENB_IP, ENB_PORT);
    Logger::mme("eNB S1 link UP ✓");
    Logger::mme("All interfaces UP — MME ready");
}

// ─────────────────────────────────────────────────────────────
// hssReceiveLoop (hss_rx_th): PRODUCER for pending_auth_
// ─────────────────────────────────────────────────────────────
void MmeNode::hssReceiveLoop() {
    Logger::mme("[hss_rx] loop started");
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
        Logger::mme("[hss_rx] ← Diameter AIA received for IMSI=" + std::to_string(imsi));
        { std::lock_guard<std::mutex> lk(pending_auth_mutex_); pending_auth_[imsi] = av; }
        pending_auth_cv_.notify_all();
    }
    Logger::mme("[hss_rx] loop stopped");
}

// ─────────────────────────────────────────────────────────────
// enbReceiveLoop (mme_th): dispatch all eNB messages
// ─────────────────────────────────────────────────────────────
void MmeNode::enbReceiveLoop() {
    Logger::mme("[mme_th] eNB receive loop started");
    while (!stop_.load()) {
        if (!enb_conn_.hasData(100)) continue;
        std::vector<uint8_t> payload;
        if (!enb_conn_.recvFrame(payload)) break;
        if (payload.size() < 8) continue;
        MessageReader r(payload);
        switch (r.msgType()) {
            case MessageType::S1AP_INITIAL_UE_MSG:          handleInitialUEMsg(payload);    break;
            case MessageType::S1AP_UL_NAS_TRANSPORT:        handleULNasTransport(payload);  break;
            case MessageType::S1AP_INITIAL_CONTEXT_SETUP_RSP: handleICSetupResponse(payload); break;
            default: Logger::warn("MME","[mme_th] unexpected: "+std::string(msg_type_str(r.msgType())));
        }
    }
    Logger::mme("[mme_th] eNB receive loop stopped");
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

    VLog::step(1, 8, "ATTACH REQUEST",
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

    Logger::mme("[mme_th] UE context created in shard[" + std::to_string(mme_id%64) +
                "]  MME-id=" + std::to_string(mme_id));
    Logger::mme("[mme_th] SHARDING: mme_id%64=" + std::to_string(mme_id%64) +
                " → only locks 1 of 64 buckets → 64x less contention than single global mutex");

    // ── STEP 2: Diameter AIR to HSS ──────────────────────────
    VLog::step(2, 8, "AUTHENTICATION-INFORMATION-REQ (AIR)",
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
    Logger::mme("[mme_th] cv.wait — blocking until HSS AIA arrives [condition_variable]");
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

    VLog::step(3, 8, "AUTHENTICATION-INFORMATION-ANS (AIA)",
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
    VLog::step(4, 8, "NAS AUTH REQUEST  (DL NAS Transport)",
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
    (void)enb_id;

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
        Logger::mme("[mme_th] ← UL NAS Transport (Auth Response 0x53)  mme_id=" + std::to_string(mme_id));
        auto ctx = ue_store_.find(mme_id);
        if (!ctx) { Logger::warn("MME","no UE context for id="+std::to_string(mme_id)); return; }
        bool ok = (res.size()>=8 && std::memcmp(res.data(), ctx->auth.xres, 8)==0);
        if (ok) {
            char res_hex[17]={};
            for(int i=0;i<8&&i<(int)res.size();i++) snprintf(res_hex+i*2,3,"%02X",res[i]);
            VLog::step(5, 8, "NAS AUTH RESPONSE  (UL NAS Transport)",
                       "UE", Logger::CLR_ENB, "eNB → MME", Logger::CLR_MME)
                .ie("RES",  std::string(res_hex) + "  (UE computed this from RAND+Ki)")
                .ie("XRES", std::string(res_hex) + "  (matches — SIM card is genuine)")
                .ie("Result","RES == XRES ✓  Authentication SUCCESS")
                .state("UE",  "AUTH_CHALLENGE → AUTHENTICATED")
                .state("MME", "CHALLENGE_SENT → SESSION_PENDING")
                .next("MME sends Security Mode Command, then Create Session to S-GW")
                .flush();
            ctx->emm_state = EmmState::SESSION_PENDING;
            handleAuthSuccess(mme_id);
        } else {
            Logger::warn("MME","AUTH FAILED for IMSI="+std::to_string(ctx->imsi));
        }
    } else if (nas_type == 0x46) {
        // Attach Complete
        Logger::mme("[mme_th] ← UL NAS Transport (Attach Complete 0x46)  mme_id=" + std::to_string(mme_id));
        auto ctx = ue_store_.find(mme_id);
        if (!ctx) return;
        ctx->emm_state = EmmState::REGISTERED;
        auto* b = ctx->defaultBearer();

        auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - ctx->attach_start).count();
        if (metrics_) metrics_->recordAttach(double(latency_ms));

        VLog::step(8, 8, "ATTACH COMPLETE  ✓  UE REGISTERED",
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
    VLog::step(6, 8, "CREATE SESSION REQUEST  (GTPv2)",
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
    Logger::mme("[mme_th] → Starting GTP-C session setup (Phase 3)");
    if (!sendCreateSession(mme_id, ctx->imsi)) return;

    // Send Initial Context Setup Request to eNB
    auto* bearer = ctx->defaultBearer();
    if (!bearer) { Logger::warn("MME","no bearer after session setup"); return; }

    Logger::mme("[mme_th] → S1AP InitialContextSetupRequest [TS 36.413 §9.1.4.1]");
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
    Logger::mme("[mme_th] → GTP-C CreateSessionReq to S-GW [TS 29.274 §7.2.1]");
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
    Logger::mme("[mme_th] waiting for S-GW CreateSessionRsp (blocking UDP recv, 5s timeout)...");
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
    Logger::mme("[mme_th] ← S-GW CreateSessionRsp — UE IP=" + std::string(ue_ip));

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

    Logger::mme("[mme_th] ← S1AP InitialContextSetupResponse  eNB S1-U TEID=" + std::to_string(enb_teid));
    Logger::ie_field("  eNB allocated TEID=" + std::to_string(enb_teid) + " for GTP-U downlink tunnel");

    // Store eNB's TEID in UE context
    auto ctx = ue_store_.find(mme_id);
    if (ctx) { auto* b=ctx->defaultBearer(); if(b) b->enb_s1u_teid=enb_teid; }

    // Send Modify Bearer Request to S-GW (tell S-GW the eNB's S1-U TEID)
    Logger::mme("[mme_th] → GTP-C ModifyBearerReq to S-GW [TS 29.274 §7.2.7]");
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
    Logger::mme("[mme_th] ← S-GW ModifyBearerRsp ✓");
    Logger::mme("[mme_th] Data path established: UE ←GTP-U→ eNB ←GTP-U→ S-GW ←GTP-U→ P-GW ←→ Internet");
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
