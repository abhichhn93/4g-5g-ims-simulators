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
};

static PcapWriter& pcap() { return PcapWriter::instance(); }

// AMF no longer hardcodes UDM's address: it's discovered via NRF at
// startup (see main()) and cached here for callUdm(). Falls back to
// UDM_HOST env / 127.0.0.1:29503 if NRF discovery doesn't find a UDM
// (e.g. udm_sim hasn't registered yet) -- logged loudly either way.
static std::string udmHost = "127.0.0.1";
static uint16_t    udmPort = 29503;

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
    std::string suci = json::get(text, "suci");

    Logger::step("Registration started: " + suci);
    Logger::amf(Level::BEGINNER, "AMF <- gNB: RegistrationRequest (ranUeNgapId=" + std::to_string(ueId) + ")");
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
    HttpMessage resp = callUdm(req);

    std::string rand     = json::get(resp.body, "rand");
    std::string autn     = json::get(resp.body, "autn");
    std::string xresStar = json::get(resp.body, "xresStar");
    std::string supi     = json::get(resp.body, "supi");

    Logger::amf(Level::ENGINEER, "UDM <- 200 OK: 5G-AKA vector for " + supi);
    Logger::ie_field("RAND  = " + rand);
    Logger::ie_field("AUTN  = " + autn);
    Logger::ie_field("XRES* = " + xresStar + "  (cached by AMF, never sent to UE)");

    ctx[ueId] = {suci, supi, xresStar};

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

    std::string out = json::obj({
        {"msgType", json::str("RegistrationAccept")},
        {"ranUeNgapId", json::num(ueId)},
        {"5gGuti", json::str(guti.str())},
        {"allowedNssai", "[{\"sst\":1,\"sd\":\"000001\"}]"},
    });
    Logger::amf(Level::BEGINNER, "AMF -> gNB: RegistrationAccept (5G-GUTI=" + guti.str() + ")");
    Logger::ie_field("Allowed NSSAI = [{sst:1, sd:\"000001\"}]");
    sendToGnb(gnb, out);
}

static void handleRegistrationComplete(const std::string& text, std::map<int, UeContext>& ctx) {
    int ueId = std::stoi(json::get(text, "ranUeNgapId"));
    std::string supi = ctx.count(ueId) ? ctx[ueId].supi : "?";
    Logger::amf(Level::BEGINNER, "AMF <- gNB: RegistrationComplete");
    Logger::step("Registration COMPLETE: " + supi + " is now registered on the 5G core");
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
            else if (msgType == "RegistrationComplete")   handleRegistrationComplete(text, ue_ctx);
            else Logger::warn(" AMF  ", "unknown N2 msgType: " + msgType);
        }
        Logger::amf(Level::BEGINNER, "gNB disconnected");
    }
}
