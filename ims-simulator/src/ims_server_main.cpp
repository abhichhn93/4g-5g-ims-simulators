#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <chrono>

#include "common/logger.h"
#include "common/pcap_writer.h"
#include "ims/pcscf_node.h"
#include "ims/scscf_node.h"
#include "ims/ims_hss.h"
#include "ims/ims_diagrams.h"
#include "ims/mtas_state.h"
#include "ims/sip_text.h"

// ============================================================
// IMS SERVER — run this FIRST in Terminal 4 (or any terminal)
//
// Starts: IMS-HSS(3870) + S-CSCF+MTAS(5070) + P-CSCF(5060)
//
// Then in THREE separate terminals:
//   Terminal 1:  ./ue_sim A
//   Terminal 2:  ./ue_sim B
//   Terminal 3:  ./ue_sim C
//
// Each UE connects to P-CSCF on port 5060.
// P-CSCF routes SIP between UEs.
//
// PCAP: ims_server_capture.pcap  (all SIP + Diameter)
//   Open in Wireshark, filter: sip || diameter
// ============================================================

static std::atomic<bool>* g_stop = nullptr;
static void sig_handler(int) { if(g_stop) g_stop->store(true); }

int main() {
    // Force line-buffered stdout so log files work when redirected to a file.
    // Without this, C++ uses 8 KB full-buffered mode when stdout is not a
    // terminal — output would be lost if the process is killed mid-run.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::cout <<
        "\n"
        "  +============================================================+\n"
        "  |  IMS SERVER — P-CSCF + S-CSCF + MTAS + IMS-HSS           |\n"
        "  |  Ports: P-CSCF:5060  S-CSCF:5070  IMS-HSS:3870            |\n"
        "  |  PCAP:  ims_server_capture.pcap  (filter: sip || diameter) |\n"
        "  +============================================================+\n"
        "\n"
        "  Start UE terminals AFTER this starts:\n"
        "    Terminal 1: ./ue_sim A\n"
        "    Terminal 2: ./ue_sim B\n"
        "    Terminal 3: ./ue_sim C\n"
        "  Press Ctrl+C to stop.\n\n";

    PcapWriter::instance().open("ims_server_capture.pcap");

    std::atomic<bool> stop{false};
    std::atomic<bool> ims_hss_ready{false}, scscf_ready{false}, pcscf_ready{false};
    g_stop = &stop;
    std::signal(SIGINT, sig_handler);

    ImsHssNode ims_hss(stop, ims_hss_ready);
    ScscfNode  scscf(stop, scscf_ready, ims_hss_ready);
    PcscfNode  pcscf(stop, pcscf_ready);

    std::thread hss_th  ([&]{ ims_hss.run(); });
    std::thread scscf_th([&]{ scscf.run();   });
    std::thread pcscf_th([&]{ pcscf.run();   });

    // Wait for all nodes to be ready
    while (!stop.load() && (!ims_hss_ready.load() || !scscf_ready.load() || !pcscf_ready.load()))
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

    if (!stop.load()) {
        Logger::sys("IMS server ready ✓ — waiting for UE-A, UE-B, UE-C to connect...");
        Logger::sys("Each UE REGISTER shows here with VLog step banners");
        Logger::sys("Ctrl+C to stop");
    }

    // Interactive: STATUS command + Ctrl+C to stop
    Logger::sys("Type STATUS to see connected UEs and active calls. Ctrl+C to stop.");
    std::string line;
    std::cout << "\nims-server> " << std::flush;
    while (!stop.load() && std::getline(std::cin, line)) {
        if (line == "STATUS" || line == "status")
            pcscf.printStatus();
        else if (line == "QUIT" || line == "quit")
            break;
        else if (line.rfind("BARR ", 0) == 0) {
            std::string id = line.substr(5);
            std::string impu = (id=="A") ? IMPU_A : (id=="B") ? IMPU_B : IMPU_C;
            MtasState::setBarred(impu, true);
            Logger::sys("BARR: " + id + " — calls to/from barred (UNBARR " + id + " to lift)");
        }
        else if (line.rfind("UNBARR ", 0) == 0) {
            std::string id = line.substr(7);
            std::string impu = (id=="A") ? IMPU_A : (id=="B") ? IMPU_B : IMPU_C;
            MtasState::setBarred(impu, false);
            Logger::sys("UNBARR: " + id + " — barring lifted");
        }
        else if (!line.empty())
            Logger::sys("Commands: STATUS  BARR A|B|C  UNBARR A|B|C  QUIT");
        if (!stop.load()) std::cout << "\nims-server> " << std::flush;
    }

    Logger::sys("Shutting down IMS server...");
    hss_th.join(); scscf_th.join(); pcscf_th.join();
    PcapWriter::instance().close();
    Logger::sys("Done. Open ims_server_capture.pcap in Wireshark — filter: sip || diameter");
    Logger::shutdown();
    return 0;
}
