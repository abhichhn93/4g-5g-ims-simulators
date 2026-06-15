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
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>

using Logger::Level;

static void doRegistration(const Socket& amf, int ueId) {
    std::string suci = ids5g::suci(ueId);
    std::string k    = aka::kFor(ids5g::msin(ueId));

    Logger::step("UE " + ids5g::supi(ueId) + " -- Registration");
    Logger::gnb(Level::BEGINNER, "gNB -> AMF: RegistrationRequest");
    Logger::ie_field("SUCI = " + suci);
    Logger::gnb(Level::INTERVIEW_T,
        "SUCI travels over the air instead of SUPI/IMSI so a passive "
        "radio eavesdropper can't track the subscriber (TS 33.501 §6.12).");

    std::string req = json::obj({
        {"msgType", json::str("RegistrationRequest")},
        {"ranUeNgapId", json::num(ueId)},
        {"suci", json::str(suci)},
        {"registrationType", json::str("initial")},
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
    Logger::sys("Commands: REG <ueId>   |   QUIT");

    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream ss(line);
        std::string cmd; ss >> cmd;
        if (cmd == "QUIT" || cmd == "quit") break;
        if (cmd == "REG" || cmd == "reg") {
            int ueId = 1; ss >> ueId;
            doRegistration(amf, ueId);
        } else if (!cmd.empty()) {
            Logger::warn(" gNB  ", "unknown command: " + line);
        }
    }
    Logger::shutdown();
}
