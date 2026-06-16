// ============================================================
// AMF — Access and Mobility Management Function
//
// REAL ROLE (TS 23.501 §6.2.1): the central signalling node of the
// 5G core. Terminates N1 (NAS, to the UE) and N2 (NGAP, to the gNB),
// and orchestrates the other core functions over the SBI:
//   - Nudm_UEAuthentication_Get / Nudm_SDM_Get  -> UDM
//   - (later) Nsmf_PDUSession_*                -> SMF
//
// THIS SIM:
//   - N2 server on TCP :38412 (the real NGAP port). Frames are
//     length-prefixed JSON (see common/wire.h n2Frame/n2Text) --
//     same simplification the 4G sim uses for S1AP-over-TCP.
//   - SBI client to UDM: real HTTP/1.1 + JSON, one connection per
//     call (see common/wire.h httpBuild/httpRecv).
//   - The only node that writes 5g_capture.pcap, since it's the one
//     that "sees" both N2 and SBI traffic.
//
// 5G ~ 4G analogy: AMF is the 5G counterpart of the 4G simulator's
// MME (src/mme/mme_node.cpp) -- the call-flow orchestrator.
// ============================================================
#include "common/socket_wrapper.h"
#include "common/wire.h"
#include "common/pcap_writer.h"
#include "common/logger.h"
#include "common/ids5g.h"
#include "common/nrf_client.h"
#include "common/json_event_log.h"
#include "common/chaos_mode.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <iomanip>
#include <map>
#include <string>

using Logger::Level;

// Per-UE state AMF needs between RegistrationRequest and
// AuthenticationResponse (and beyond). Keyed by ranUeNgapId, the
// gNB-assigned UE reference carried on N2 (TS 38.413).
//
// INTERVIEW NOTE: in a real, horizontally-scaled AMF this map would
// live in a shared datastore (UDSF, TS 23.501 §5.4.7.5) so ANY AMF
// replica can pick up the next message for this UE -- that's what
// makes "multiple AMF instances" possible. Here it's just an
// in-memory std::map because this sim runs one AMF process.
struct UeContext {
    std::string suci;
    std::string supi;
    std::string xresStar;
    std::string nssai;   // requested S-NSSAI JSON, e.g. {"sst":2,"sd":"000002"}
};

static PcapWriter& pcap() { return PcapWriter::instance(); }

// AMF no longer hardcodes UDM's address: it's discovered via NRF at
// startup (see main()) and cached here for callUdm(). Falls back to
// UDM_HOST env / 127.0.0.1:29503 if NRF discovery doesn't find a UDM
// (e.g. udm_sim hasn't registered yet) -- logged loudly either way.
static std::string udmHost = "127.0.0.1";
static uint16_t    udmPort = 29503;
static std::string smfHost = "127.0.0.1";
static uint16_t    smfPort = 29502;

// Open a fresh SBI connection to UDM, send `req`, return the response.
static HttpMessage callUdm(const std::string& req) {
    Socket udm = Socket::connectTo(udmHost.c_str(), udmPort);
    Logger::raw(req);
    pcap().writeAppText(req, PcapWriter::IP_AMF, PcapWriter::PORT_SBI,
                              PcapWriter::IP_UDM, PcapWriter::PORT_SBI);
    httpSend(udm, req);
    HttpMessage resp;
    httpRecv(udm, resp);
    Logger::raw(resp.body);
    pcap().writeAppText(httpBuild(resp.startLine, resp.body),
                         PcapWriter::IP_UDM, PcapWriter::PORT_SBI,
                         PcapWriter::IP_AMF, PcapWriter::PORT_SBI);
    return resp;
}

// AMF → SMF: Nsmf_PDUSession_CreateSMContext (TS 29.502 §4.2.2.2)
static std::string callSmf(const std::string& supi, int pdu_sess_id, const std::string& dnn) {
    Logger::amf(Level::BEGINNER,
        "AMF -> SMF: Nsmf_PDUSession_CreateSMContext [TS 29.502 §4.2.2.2]");
    Logger::ie_field("  SMF will allocate UE IP, set up N4 (PFCP) session with UPF");

    Logger::amf(Level::INTERVIEW_T,
        "[INTERVIEW] Q: Why does AMF call SMF separately, not handle PDU sessions itself?");
    Logger::amf(Level::INTERVIEW_C,
        "A: Separation of concerns. AMF handles mobility (N2, NAS), SMF handles");
    Logger::amf(Level::INTERVIEW_C,
        "   sessions (IP allocation, UPF selection, policy). This allows independent");
    Logger::amf(Level::INTERVIEW_C,
        "   scaling: more simultaneous PDU sessions → scale SMF, not AMF.");

    std::string body = json::obj({
        {"supi",         json::str(supi)},
        {"pduSessionId", json::num(pdu_sess_id)},
        {"dnn",          json::str(dnn.empty() ? "internet" : dnn)},
        {"snssai",       json::str("{\"sst\":1,\"sd\":\"000001\"}")},
        {"servingNssai", json::str("5G:mnc010.mcc404.3gppnetwork.org")},
        {"anType",       json::str("3GPP_ACCESS")},
    });
    std::string req_str = httpBuild(
        "POST /nsmf-pdusession/v1/sm-contexts HTTP/1.1",
        body,
        "Host: smf.5gc.mnc010.mcc404.3gppnetwork.org");

    try {
        Socket smf = Socket::connectTo(smfHost.c_str(), smfPort);
        Logger::raw(req_str);
        pcap().writeAppText(req_str, PcapWriter::IP_AMF, PcapWriter::PORT_SBI,
                                      PcapWriter::IP_SMF, PcapWriter::PORT_SBI);
        httpSend(smf, req_str);
        HttpMessage resp;
        if (!httpRecv(smf, resp)) {
            Logger::warn(" AMF  ", "SMF closed connection without response");
            return "";
        }
        Logger::raw(resp.body);
        pcap().writeAppText(httpBuild(resp.startLine, resp.body),
                             PcapWriter::IP_SMF, PcapWriter::PORT_SBI,
                             PcapWriter::IP_AMF, PcapWriter::PORT_SBI);
        std::string ue_ip = json::get(resp.body, "ueIpAddress");
        Logger::amf(Level::BEGINNER,
            "AMF <- SMF: 201 Created  ueIp=" + ue_ip);
        Logger::ie_field("  UPF GTP-U TEID=0x00000101 (gNB will create GTP-U tunnel to UPF)");
        JsonEventLog::logEvent("AMF", "SMF", "HTTP SMF PDUSession", "N11/SBI", 29502, "5g");
        return ue_ip;
    } catch (const std::exception& e) {
        Logger::warn(" AMF  ", "SMF unreachable: " + std::string(e.what()) +
                     " — PDU session skipped (run smf_sim first)");
        return "";
    }
}

static void sendToGnb(const Socket& gnb, const std::string& json_text) {
    Logger::raw(json_text);
    pcap().writeAppText(json_text, PcapWriter::IP_AMF, PcapWriter::PORT_N2,
                                    PcapWriter::IP_GNB, PcapWriter::PORT_N2);
    gnb.sendFrame(n2Frame(json_text));
}

// Step 1-4: RegistrationRequest -> Nudm_UEAuthentication_Get -> AuthenticationRequest
static void handleRegistrationRequest(const Socket& gnb, const std::string& text,
                                       std::map<int, UeContext>& ctx) {
    int ueId = std::stoi(json::get(text, "ranUeNgapId"));
    std::string suci  = json::get(text, "suci");
    std::string nssai = json::get(text, "requestedNssai");  // e.g. {"sst":2,"sd":"000002"}

    // Derive slice name for logging
    std::string sliceName = "eMBB";
    if (nssai.find("\"sst\":2") != std::string::npos) sliceName = "URLLC";
    else if (nssai.find("\"sst\":1") != std::string::npos) sliceName = "eMBB";

    Logger::step("Registration started: " + suci);
    Logger::amf(Level::BEGINNER, "AMF <- gNB: RegistrationRequest (ranUeNgapId=" + std::to_string(ueId) + ")");
    Logger::ie_field("Requested NSSAI = " + (nssai.empty() ? "{sst:1}" : nssai) + "  (slice: " + sliceName + ")");
    { std::string sst = (sliceName == "URLLC") ? "2" : "1";
      Logger::amf(Level::INTERVIEW_T,
        std::string("AMF checks Subscribed NSSAI from UDM vs Requested NSSAI. "
        "Only Allowed NSSAI is returned in RegistrationAccept. "
        "NRF discovery is then filtered by the allowed slice — "
        "NRF returns only SMFs/UPFs that serve SST=") + sst); }
    Logger::ie_field("SUCI = " + suci);
    Logger::amf(Level::INTERVIEW_C,
        "AMF stores a UeContext{suci,supi,xresStar} for this ranUeNgapId in "
        "ue_ctx_ -- a real AMF keeps this in shared storage (UDSF) so any "
        "AMF replica can continue the procedure (this is HOW 'multiple AMF "
        "instances' work).");

    std::string body = json::obj({{"servingNetworkName", json::str("5G:" + ids5g::plmnDomain() + ".3gppnetwork.org")}});
    std::string req = httpBuild("POST /nudm-ueau/v2/" + suci + "/security-information/generate-auth-data HTTP/1.1",
                                 body, "Host: udm.5gc." + ids5g::plmnDomain() + ".3gppnetwork.org");
    Logger::amf(Level::BEGINNER, "AMF -> UDM: Nudm_UEAuthentication_Get (SBI/HTTP)");
    JsonEventLog::logEvent("AMF", "UDM", "HTTP UDM Authentication", "N8/SBI", 29503, "5g");

    // CHAOS: random NRF/UDM discovery drop (20%)
    if (Chaos::rollDrop("Nudm_UEAuthentication_Get", "AMF", "UDM",
        "AMF implements exponential backoff (RFC 6585): 1s → 2s → 4s retries. "
        "After 3 failures, Registration Reject (cause: UDM unreachable) is sent.")) {
        std::string reject = json::obj({{"msgType", json::str("RegistrationReject")},
                                         {"ranUeNgapId", json::str("0")},
                                         {"cause", json::str("chaos-udm-drop")}});
        sendToGnb(gnb, reject);
        return;
    }

    HttpMessage resp = callUdm(req);

    std::string rand     = json::get(resp.body, "rand");
    std::string autn     = json::get(resp.body, "autn");
    std::string xresStar = json::get(resp.body, "xresStar");
    std::string supi     = json::get(resp.body, "supi");

    Logger::amf(Level::ENGINEER, "UDM <- 200 OK: 5G-AKA vector for " + supi);
    Logger::ie_field("RAND  = " + rand);
    Logger::ie_field("AUTN  = " + autn);
    Logger::ie_field("XRES* = " + xresStar + "  (cached by AMF, never sent to UE)");

    ctx[ueId] = {suci, supi, xresStar, nssai};

    std::string out = json::obj({
        {"msgType", json::str("AuthenticationRequest")},
        {"ranUeNgapId", json::num(ueId)},
        {"rand", json::str(rand)},
        {"autn", json::str(autn)},
    });
    Logger::amf(Level::BEGINNER, "AMF -> gNB: AuthenticationRequest (RAND, AUTN)");
    sendToGnb(gnb, out);
}

// Step 5-9: AuthenticationResponse -> verify -> Nudm_SDM_Get -> RegistrationAccept
static void handleAuthResponse(const Socket& gnb, const std::string& text,
                                std::map<int, UeContext>& ctx) {
    int ueId = std::stoi(json::get(text, "ranUeNgapId"));
    std::string resStar = json::get(text, "resStar");

    auto it = ctx.find(ueId);
    if (it == ctx.end()) {
        Logger::warn(" AMF  ", "AuthenticationResponse for unknown ranUeNgapId=" + std::to_string(ueId));
        return;
    }
    UeContext& c = it->second;

    Logger::amf(Level::BEGINNER, "AMF <- gNB: AuthenticationResponse");
    Logger::ie_field("RES*  = " + resStar);

    if (resStar != c.xresStar) {
        Logger::warn(" AMF  ", "RES* != XRES* for " + c.suci + " -- authentication FAILED");
        std::string out = json::obj({
            {"msgType", json::str("RegistrationReject")},
            {"ranUeNgapId", json::num(ueId)},
            {"cause", json::str("authentication-failure")},
        });
        sendToGnb(gnb, out);
        return;
    }

    Logger::amf(Level::BEGINNER, "RES* == XRES* -- UE authenticated.");
    Logger::amf(Level::INTERVIEW_T,
        "5G-AKA mutual authentication (TS 33.501 §6.1.3.2): matching RES*/XRES* "
        "proves the UE holds the same long-term key K as UDM/ARPF.");

    std::string req = httpBuild("GET /nudm-sdm/v2/" + c.supi + "/am-data HTTP/1.1",
                                 "", "Host: udm.5gc." + ids5g::plmnDomain() + ".3gppnetwork.org");
    Logger::amf(Level::BEGINNER, "AMF -> UDM: Nudm_SDM_Get (am-data)");
    HttpMessage resp = callUdm(req);
    Logger::amf(Level::ENGINEER, "UDM <- 200 OK: " + resp.body);

    std::ostringstream guti;
    guti << "5g-guti-" << ids5g::mcc() << "-" << ids5g::mnc() << "-amf01-"
         << std::setw(5) << std::setfill('0') << ueId;

    // Echo back the requested NSSAI as allowed (real AMF also checks subscription)
    const std::string& allowedNssai = c.nssai.empty() ? "[{\"sst\":1,\"sd\":\"000001\"}]"
                                                       : "[" + c.nssai + "]";
    std::string out = json::obj({
        {"msgType", json::str("RegistrationAccept")},
        {"ranUeNgapId", json::num(ueId)},
        {"5gGuti", json::str(guti.str())},
        {"allowedNssai", allowedNssai},
    });
    Logger::amf(Level::BEGINNER, "AMF -> gNB: RegistrationAccept (5G-GUTI=" + guti.str() + ")");
    Logger::ie_field("Allowed NSSAI = " + allowedNssai);
    JsonEventLog::logEvent("AMF", "gNB", "HTTP AMF Registration", "N2/NGAP", 38412, "5g");
    sendToGnb(gnb, out);
}

static void handleRegistrationComplete(const Socket& gnb, const std::string& text,
                                       std::map<int, UeContext>& ctx) {
    int ueId = std::stoi(json::get(text, "ranUeNgapId"));
    std::string supi = ctx.count(ueId) ? ctx[ueId].supi : "?";
    Logger::amf(Level::BEGINNER, "AMF <- gNB: RegistrationComplete");
    Logger::step("Registration COMPLETE: " + supi + " is now registered on the 5G core");
}

// Handle PDU Session Establishment Request from gNB (forwarded from UE NAS)
static void handlePduSessionRequest(const Socket& gnb, const std::string& text,
                                    std::map<int, UeContext>& ctx) {
    int ueId       = std::stoi(json::get(text, "ranUeNgapId"));
    int pduSessId  = 1;
    std::string ps = json::get(text, "pduSessionId");
    if (!ps.empty()) pduSessId = std::stoi(ps);
    std::string dnn = json::get(text, "dnn");
    std::string supi = ctx.count(ueId) ? ctx[ueId].supi : ("imsi-404100000000" + std::to_string(ueId));

    Logger::amf(Level::BEGINNER,
        "AMF <- gNB: PDU Session Establishment Request (NAS over N2/NGAP)");
    Logger::ie_field("  SUPI         = " + supi);
    Logger::ie_field("  PDU Session  = " + std::to_string(pduSessId));
    Logger::ie_field("  DNN          = " + (dnn.empty() ? "internet" : dnn));

    Logger::amf(Level::INTERVIEW_T,
        "[INTERVIEW] Q: What triggers PDU Session Establishment?");
    Logger::amf(Level::INTERVIEW_C,
        "A: UE sends NAS: PDU Session Establishment Request (msg_type=0xC1).");
    Logger::amf(Level::INTERVIEW_C,
        "   gNB wraps it in NGAP: UplinkNASTransport → AMF.");
    Logger::amf(Level::INTERVIEW_C,
        "   AMF identifies the SM procedure, forwards to SMF: Nsmf_PDUSession_CreateSMContext.");
    Logger::amf(Level::INTERVIEW_C,
        "   SMF programs UPF via PFCP, returns UE IP.");
    Logger::amf(Level::INTERVIEW_C,
        "   AMF sends NGAP: PDU Session Resource Setup Request to gNB with UE IP + UPF TEID.");

    // Call SMF to create PDU session
    std::string ue_ip = callSmf(supi, pduSessId, dnn);
    if (ue_ip.empty()) ue_ip = "10.45.0.2"; // fallback for demo

    // Send PDU Session Accept back to gNB
    std::string out = json::obj({
        {"msgType",      json::str("PduSessionAccept")},
        {"ranUeNgapId",  json::num(ueId)},
        {"pduSessionId", json::num(pduSessId)},
        {"ueIpAddress",  json::str(ue_ip)},
        {"upfIp",        json::str("10.1.0.4")},
        {"upfTeid",      json::str("0x00000101")},
        {"dnn",          json::str(dnn.empty() ? "internet" : dnn)},
    });
    Logger::amf(Level::BEGINNER,
        "AMF -> gNB: PDU Session Resource Setup Request  ueIp=" + ue_ip);
    Logger::ie_field("  gNB will create GTP-U tunnel to UPF (10.1.0.4) using TEID=0x00000101");
    Logger::ie_field("  User plane is now established: UE ←(radio)→ gNB ←(GTP-U N3)→ UPF ←(N6)→ internet");
    sendToGnb(gnb, out);
}

int main() {
    Logger::setSessionFile("g5_amf_session.log");
    Logger::setLevelFromEnv();

    const uint16_t N2_PORT = 38412;
    pcap().open("5g_capture.pcap");

    Logger::step("AMF starting");

    // Open the N2 listening socket FIRST, before any NRF round-trips: a
    // gNB connecting right at startup must not see "connection refused"
    // just because the NRF is slow/unreachable (gnb_sim's connectTo has
    // no retry). accept() isn't called until the loop below, so the OS
    // queues the gNB's connection in the meantime.
    Socket server = Socket::createServer("0.0.0.0", N2_PORT);
    Logger::sys("AMF: Access & Mobility Management Function listening on N2 :" + std::to_string(N2_PORT));

    // AMF_SELF_HOST lets this binary tell the NRF where it can be reached:
    // locally "127.0.0.1", but a container/pod sets it to its own Docker
    // Compose service name / K8s Service DNS name (e.g. "amf-sim"). AMF's
    // only listening port in this sim is its N2 port.
    const char* AMF_SELF_HOST = std::getenv("AMF_SELF_HOST") ? std::getenv("AMF_SELF_HOST") : "127.0.0.1";
    nrfclient::registerSelf(Logger::CLR_AMF, " AMF  ", "amf-1", "AMF", AMF_SELF_HOST, N2_PORT);

    // This IS the "how does NRF communication happen" demo: instead of a
    // hardcoded UDM_HOST, AMF asks the NRF "who is the UDM?" and caches
    // the answer.
    auto udm = nrfclient::discover(Logger::CLR_AMF, " AMF  ", "UDM");
    if (udm.found) {
        udmHost = udm.host;
        udmPort = udm.port;
    } else {
        udmHost = std::getenv("UDM_HOST") ? std::getenv("UDM_HOST") : "127.0.0.1";
        udmPort = 29503;
        Logger::sys("AMF: NRF discovery for UDM failed -- falling back to " + udmHost + ":" + std::to_string(udmPort));
    }
    Logger::sys("AMF: SBI peer UDM at " + udmHost + ":" + std::to_string(udmPort));

    // Discover SMF via NRF (for PDU session handling)
    auto smf = nrfclient::discover(Logger::CLR_AMF, " AMF  ", "SMF");
    if (smf.found) {
        smfHost = smf.host;
        smfPort = smf.port;
    } else {
        smfHost = std::getenv("SMF_HOST") ? std::getenv("SMF_HOST") : "127.0.0.1";
        smfPort = 29502;
        Logger::sys("AMF: NRF discovery for SMF failed -- falling back to " + smfHost + ":" + std::to_string(smfPort));
    }
    Logger::sys("AMF: SBI peer SMF at " + smfHost + ":" + std::to_string(smfPort));

    std::map<int, UeContext> ue_ctx;

    while (true) {
        Socket gnb = server.accept();
        Logger::amf(Level::BEGINNER, "gNB connected over N2");

        while (true) {
            std::vector<uint8_t> payload;
            if (!gnb.recvFrame(payload)) break;
            std::string text = n2Text(payload);
            std::string msgType = json::get(text, "msgType");
            Logger::raw(text);

            pcap().writeAppText(text, PcapWriter::IP_GNB, PcapWriter::PORT_N2,
                                       PcapWriter::IP_AMF, PcapWriter::PORT_N2);

            if (msgType == "RegistrationRequest")    handleRegistrationRequest(gnb, text, ue_ctx);
            else if (msgType == "AuthenticationResponse") handleAuthResponse(gnb, text, ue_ctx);
            else if (msgType == "RegistrationComplete")   handleRegistrationComplete(gnb, text, ue_ctx);
            else if (msgType == "PduSessionRequest")      handlePduSessionRequest(gnb, text, ue_ctx);
            else Logger::warn(" AMF  ", "unknown N2 msgType: " + msgType);
        }
        Logger::amf(Level::BEGINNER, "gNB disconnected");
    }
}
