// ============================================================
// gNB — simulated gNodeB + UE, driven by an interactive CLI
//
// REAL ROLE: the gNB terminates the radio link to the UE and
// relays NAS messages to the AMF over N2/N1. For this simulator we
// collapse "gNB + UE" into one process -- the CLI commands play
// the role of the UE deciding to register, and this binary speaks
// N2 to amf_sim.
//
// COMMANDS:
//   REG <n>   Run a full Registration procedure for UE #n
//             (SUPI = imsi-404100000000<0n>, see common/ids5g.h)
//   QUIT
//
// 5G ~ 4G analogy: gnb_sim is the 5G counterpart of the 4G
// simulator's eNB + ue_sim (src/enb/enb_node.cpp, src/ue_sim.cpp).
// ============================================================
#include "common/socket_wrapper.h"
#include "common/wire.h"
#include "common/aka_lite.h"
#include "common/ids5g.h"
#include "common/logger.h"
#include "common/chaos_mode.h"
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

using Logger::Level;

static void doRegistration(const Socket& amf, int ueId, const std::string& slice = "embb") {
    std::string suci = ids5g::suci(ueId);
    std::string k    = aka::kFor(ids5g::msin(ueId));

    // NSSAI: SST=1 eMBB, SST=2 URLLC  (TS 23.501 §5.15)
    int sst = (slice == "urllc") ? 2 : 1;
    std::string sd  = (sst == 2) ? "000002" : "000001";

    Logger::step("UE " + ids5g::supi(ueId) + " -- Registration [slice=" + slice + "]");
    Logger::gnb(Level::BEGINNER, "gNB -> AMF: RegistrationRequest");
    Logger::ie_field("SUCI = " + suci);
    Logger::ie_field("Requested NSSAI: SST=" + std::to_string(sst) + " SD=" + sd +
                     "  (" + slice + " slice)");
    Logger::gnb(Level::INTERVIEW_T,
        "SUCI travels over the air instead of SUPI/IMSI so a passive "
        "radio eavesdropper can't track the subscriber (TS 33.501 §6.12).");
    Logger::gnb(Level::INTERVIEW_T,
        "NSSAI (Network Slice Selection Assistance Information) tells AMF which slice to use. "
        "SST=1 eMBB (enhanced Mobile Broadband), SST=2 URLLC (Ultra-Reliable Low-Latency). "
        "AMF queries NRF for NFs belonging to the requested slice. Ref: TS 23.501 §5.15.2.");

    std::string nssai = "{\"sst\":" + std::to_string(sst) + ",\"sd\":\"" + sd + "\"}";
    std::string req = json::obj({
        {"msgType", json::str("RegistrationRequest")},
        {"ranUeNgapId", json::num(ueId)},
        {"suci", json::str(suci)},
        {"registrationType", json::str("initial")},
        {"requestedNssai", json::str(nssai)},
    });
    Logger::raw(req);
    amf.sendFrame(n2Frame(req));

    std::vector<uint8_t> payload;
    if (!amf.recvFrame(payload)) { Logger::warn(" gNB  ", "AMF closed connection"); return; }
    std::string text = n2Text(payload);
    Logger::raw(text);
    if (json::get(text, "msgType") != "AuthenticationRequest") {
        Logger::warn(" gNB  ", "expected AuthenticationRequest, got: " + text);
        return;
    }
    std::string rand = json::get(text, "rand");
    std::string autn = json::get(text, "autn");
    Logger::gnb(Level::BEGINNER, "gNB <- AMF: AuthenticationRequest");
    Logger::ie_field("RAND = " + rand);
    Logger::ie_field("AUTN = " + autn);

    std::string resStar = aka::byteXor(rand, k);
    Logger::gnb(Level::ENGINEER, "UE computes RES* from RAND and its own K (same K as UDM holds)");
    Logger::ie_field("RES*  = " + resStar);
    Logger::gnb(Level::INTERVIEW_C,
        "doRegistration() is a straight-line function -- one TCP request/response "
        "per step, no callbacks/threads needed because gnb_sim only talks to ONE "
        "AMF connection at a time. Compare to amf_sim, which fans out to UDM "
        "per-request over a fresh SBI connection.");

    std::string resp = json::obj({
        {"msgType", json::str("AuthenticationResponse")},
        {"ranUeNgapId", json::num(ueId)},
        {"resStar", json::str(resStar)},
    });
    Logger::gnb(Level::BEGINNER, "gNB -> AMF: AuthenticationResponse");
    Logger::raw(resp);
    amf.sendFrame(n2Frame(resp));

    if (!amf.recvFrame(payload)) { Logger::warn(" gNB  ", "AMF closed connection"); return; }
    text = n2Text(payload);
    Logger::raw(text);
    std::string msgType = json::get(text, "msgType");

    if (msgType == "RegistrationReject") {
        Logger::warn(" gNB  ", "Registration REJECTED: cause=" + json::get(text, "cause"));
        return;
    }
    if (msgType != "RegistrationAccept") {
        Logger::warn(" gNB  ", "expected RegistrationAccept, got: " + text);
        return;
    }
    std::string guti = json::get(text, "5gGuti");
    Logger::gnb(Level::BEGINNER, "gNB <- AMF: RegistrationAccept");
    Logger::ie_field("5G-GUTI = " + guti);
    Logger::ie_field("Allowed NSSAI received (S-NSSAI sst=1 / eMBB slice)");

    std::string done = json::obj({
        {"msgType", json::str("RegistrationComplete")},
        {"ranUeNgapId", json::num(ueId)},
    });
    Logger::gnb(Level::BEGINNER, "gNB -> AMF: RegistrationComplete");
    Logger::raw(done);
    amf.sendFrame(n2Frame(done));

    Logger::step("UE " + ids5g::supi(ueId) + " registration finished -- 5G-GUTI=" + guti);
}

// PDU Session Establishment: UE requests data connectivity (TS 23.502 §4.3.2)
static void doPduSession(const Socket& amf, int ueId, int pduSessId = 1,
                          const std::string& dnn = "internet") {
    Logger::step("UE " + ids5g::supi(ueId) + " -- PDU Session Establishment");
    Logger::gnb(Level::BEGINNER, "gNB -> AMF: PDU Session Establishment Request (NAS via N2)");
    Logger::ie_field("PDU Session ID = " + std::to_string(pduSessId));
    Logger::ie_field("DNN            = " + dnn + "  (Data Network Name = APN equivalent in 5G)");
    Logger::ie_field("S-NSSAI        = {sst:1, sd:000001}  (eMBB slice)");

    Logger::gnb(Level::INTERVIEW_T,
        "INTERVIEW: PDU Session = bearer context in 5G. Default bearer equivalent.");
    Logger::gnb(Level::INTERVIEW_C,
        "Each UE can have multiple concurrent PDU Sessions to different DNNs.");
    Logger::gnb(Level::INTERVIEW_C,
        "Unlike 4G where bearer QoS is managed by P-GW+PCRF, in 5G SMF+PCF handle it.");

    std::string req = json::obj({
        {"msgType",      json::str("PduSessionRequest")},
        {"ranUeNgapId",  json::num(ueId)},
        {"pduSessionId", json::num(pduSessId)},
        {"dnn",          json::str(dnn)},
        {"sNssai",       json::str("{\"sst\":1,\"sd\":\"000001\"}")},
    });
    Logger::raw(req);
    amf.sendFrame(n2Frame(req));

    std::vector<uint8_t> payload;
    if (!amf.recvFrame(payload)) { Logger::warn(" gNB  ", "AMF closed connection"); return; }
    std::string text = n2Text(payload);
    Logger::raw(text);

    if (json::get(text, "msgType") != "PduSessionAccept") {
        Logger::warn(" gNB  ", "expected PduSessionAccept, got: " + text);
        return;
    }

    std::string ue_ip  = json::get(text, "ueIpAddress");
    std::string upf_ip = json::get(text, "upfIp");
    std::string upf_teid = json::get(text, "upfTeid");

    Logger::gnb(Level::BEGINNER, "gNB <- AMF: PDU Session Resource Setup (ueIp=" + ue_ip + ")");
    Logger::ie_field("UE IPv4   = " + ue_ip + "  (assigned by SMF from DNN pool 10.45.0.0/16)");
    Logger::ie_field("UPF N3 IP = " + upf_ip + "  (gNB creates GTP-U tunnel to this address)");
    Logger::ie_field("UPF TEID  = " + upf_teid + "  (GTP-U tunnel endpoint identifier)");
    Logger::gnb(Level::ENGINEER,
        "gNB programs its GTP-U engine: UL: {UE traffic} → encapsulate GTP-U → UPF " + upf_ip);
    Logger::gnb(Level::ENGINEER,
        "DL: GTP-U from UPF → decapsulate → send to UE as plain IP");
    Logger::gnb(Level::INTERVIEW_C,
        "User plane path: UE ←(radio)→ gNB ←(N3 GTP-U)→ UPF ←(N6)→ internet");

    Logger::step("PDU SESSION ESTABLISHED: UE has IP=" + ue_ip + "  DNN=" + dnn);
}

int main() {
    Logger::setSessionFile("g5_gnb_session.log");
    Logger::setLevelFromEnv();

    // AMF_HOST lets this binary run unchanged in Docker/K8s: locally it
    // defaults to "127.0.0.1", but a container/pod sets AMF_HOST to the
    // Docker Compose service name or K8s Service DNS name (e.g. "amf-sim").
    const char* AMF_IP = std::getenv("AMF_HOST") ? std::getenv("AMF_HOST") : "127.0.0.1";
    const uint16_t AMF_PORT = 38412;

    Logger::step("gNB starting");
    Logger::sys("gNB: connecting to AMF at " + std::string(AMF_IP) + ":" + std::to_string(AMF_PORT));
    Socket amf = Socket::connectTo(AMF_IP, AMF_PORT);
    Logger::sys("gNB: N2 connection established");
    Logger::sys("Commands: REG <ueId>  |  PDU <ueId> [pduSessId] [dnn]  |  QUIT");

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;
        for (auto& c : cmd) c = char(std::toupper(unsigned(c)));
        if (cmd == "QUIT") break;
        if (cmd == "REG") {
            int ueId = 1; ss >> ueId;
            std::string slice = "embb";
            std::string token;
            while (ss >> token) {
                if (token == "--slice") { ss >> slice; for (auto& c : slice) c = char(std::tolower(unsigned(c))); }
            }
            if (slice != "embb" && slice != "urllc") {
                Logger::sys("Usage: REG <ueId> [--slice embb|urllc]"); slice = "embb";
            }
            doRegistration(amf, ueId, slice);
        } else if (cmd == "PDU") {
            int ueId = 1, pduSessId = 1; std::string dnn = "internet";
            ss >> ueId;
            if (ss >> pduSessId) { std::string d; if (ss >> d) dnn = d; }
            doPduSession(amf, ueId, pduSessId, dnn);
        } else if (cmd == "CHAOS") {
            std::string arg; ss >> arg;
            for (auto& c : arg) c = char(std::toupper(unsigned(c)));
            Chaos::setEnabled(arg == "ON");
        } else if (!cmd.empty()) {
            Logger::sys("Commands: REG <ueId>  |  PDU <ueId> [pduSessId] [dnn]  |  CHAOS <on|off>  |  QUIT");
        }
    }
    Logger::shutdown();
}
