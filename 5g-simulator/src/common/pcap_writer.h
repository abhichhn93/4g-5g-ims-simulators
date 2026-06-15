#pragma once
#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <set>
#include <fstream>
#include <vector>
#include <atomic>

// ============================================================
// PCAP WRITER (5G core) — trimmed down from the 4G simulator's
// common/pcap_writer.cpp to the one thing this project needs:
// "write some text as a TCP packet, with a real 3-way handshake
// so Wireshark accepts the stream."
//
//   N2  (gNB <-> AMF)        TCP port 38412 (the REAL NGAP/SCTP
//                             port number) carrying readable JSON.
//                             No NGAP dissector needed — the bytes
//                             pane shows the JSON as plain text.
//   SBI (AMF <-> UDM/SMF/..) TCP port 80 carrying real HTTP/1.1 +
//                             JSON — Wireshark's built-in "HTTP"
//                             dissector parses this natively. This
//                             matches the REAL 3GPP SBA design
//                             (TS 29.500: SBI interfaces are
//                             HTTP/2 + JSON).
//
// Only amf_sim links this — it's the one node that "sees" both N2
// and SBI traffic, so it's also the one that writes 5g_capture.pcap.
// ============================================================
class PcapWriter {
public:
    static PcapWriter& instance() { static PcapWriter p; return p; }

    void open(const std::string& filename = "5g_capture.pcap");
    void close();

    // Send `text` as one TCP segment src->dst (writes a SYN/SYN-ACK/ACK
    // handshake first if this is a new connection).
    void writeAppText(const std::string& text,
                       uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port);

    // Node IPs used in pcap frames — 10.1.0.x range, separate from the
    // 4G simulator's 10.0.0.x range so a future combined 4G+5G capture
    // can tell the two cores apart at a glance.
    static constexpr uint32_t IP_GNB = 0x0A010001; // 10.1.0.1
    static constexpr uint32_t IP_AMF = 0x0A010002; // 10.1.0.2
    static constexpr uint32_t IP_SMF = 0x0A010003; // 10.1.0.3
    static constexpr uint32_t IP_UPF = 0x0A010004; // 10.1.0.4
    static constexpr uint32_t IP_UDM = 0x0A010005; // 10.1.0.5
    static constexpr uint32_t IP_NRF = 0x0A010006; // 10.1.0.6

    static constexpr uint16_t PORT_N2  = 38412; // real NGAP port
    static constexpr uint16_t PORT_SBI = 80;    // -> Wireshark "HTTP"

private:
    PcapWriter() = default;
    ~PcapWriter() { close(); }

    void writeGlobalHeader();
    void writePacket(const std::vector<uint8_t>& frame);

    void ensureTcpHandshake(uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port);

    std::vector<uint8_t> buildEthernet();
    std::vector<uint8_t> buildIPTCP(uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port,
                                     const std::vector<uint8_t>& payload,
                                     uint8_t tcp_flags,
                                     uint32_t seq_num,
                                     uint32_t ack_num);

    bool     isClient  (uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port);
    uint32_t nextSeq   (uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port);
    uint32_t peerSeq   (uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port);
    void     advanceSeq(uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, uint32_t bytes);

    static void     putU16  (std::vector<uint8_t>& v, uint16_t x);
    static void     putU32le(std::vector<uint8_t>& v, uint32_t x);
    static uint16_t ipChecksum(const uint8_t* buf, int len);

    // Sorted (src,dst) pair -> one key, so A->B and B->A share a handshake.
    static uint64_t connKey(uint32_t a_ip, uint16_t a_port,
                             uint32_t b_ip, uint16_t b_port) {
        uint64_t a = (uint64_t(a_ip) << 16) | a_port;
        uint64_t b = (uint64_t(b_ip) << 16) | b_port;
        return (a < b) ? (a << 32 | b) : (b << 32 | a);
    }

    struct ConnSeq { uint32_t client_seq{1000}; uint32_t server_seq{2000}; };

    std::ofstream               file_;
    std::mutex                  mtx_;
    std::atomic<uint32_t>       pkt_id_{1};
    std::map<uint64_t, ConnSeq> conn_seq_;
    std::set<uint64_t>          tcp_started_;
    bool                        open_{false};
};
