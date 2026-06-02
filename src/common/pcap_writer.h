#pragma once
// ============================================================
// PCAP WRITER — all 8 attach steps visible in Wireshark
//
// Protocols shown:
//   Diameter  port 3868/3869  → shows "Diameter" (S6a AIR/AIA, Gx CCR/CCA)
//   GTPv2     port 2123/2124  → shows "GTPv2"    (Create/Modify/Delete Session)
//   SIP       port 5060       → shows "SIP"       (REGISTER, INVITE, 200 OK)
//   S1AP      port 36412      → shows "TCP" + Lua dissector decodes it
//
// TCP SYN FIX: Diameter requires TCP. Wireshark only applies Diameter
// dissector if it sees a proper TCP stream. We write fake SYN/SYN-ACK/ACK
// before the first data packet on each new TCP connection.
// ============================================================
#include <cstdint>
#include <string>
#include <map>
#include <mutex>
#include <set>
#include <fstream>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>

enum class DiameterCmd : uint32_t {
    CREDIT_CONTROL    = 272,   // Gx CCR/CCA
    AUTH_INFO         = 318,   // S6a AIR/AIA
    UPDATE_LOCATION   = 316,   // S6a ULR/ULA
    SERVER_ASSIGNMENT = 301,   // Cx SAR/SAA
};

enum class DiameterApp : uint32_t {
    S6A = 16777251,
    GX  = 16777238,
    CX  = 16777216,
};

enum class GtpMsgType : uint8_t {
    CREATE_SESSION_REQ = 32,
    CREATE_SESSION_RSP = 33,
    MODIFY_BEARER_REQ  = 34,
    MODIFY_BEARER_RSP  = 35,
    DELETE_SESSION_REQ = 36,
    DELETE_SESSION_RSP = 37,
};

class PcapWriter {
public:
    static PcapWriter& instance() { static PcapWriter p; return p; }

    void open(const std::string& filename = "mme_capture.pcap");
    void close();

    // Diameter — writes SYN handshake first if new connection
    void writeDiameter(DiameterCmd cmd, DiameterApp app, bool is_request,
                       uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port,
                       const std::vector<uint8_t>& avp_data = {});

    // GTPv2 over UDP
    void writeGTPv2(GtpMsgType msg_type, uint32_t teid,
                    uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port,
                    const std::vector<uint8_t>& ie_data = {});

    // SIP text over TCP — writes SYN first if new connection
    void writeSIP(const std::string& sip_text,
                  uint32_t src_ip, uint16_t src_port,
                  uint32_t dst_ip, uint16_t dst_port);

    // S1AP (our custom TLV) over TCP port 36412
    // Wireshark shows TCP — Lua dissector decodes message names
    void writeS1AP(const std::string& msg_name,
                   uint32_t src_ip, uint16_t src_port,
                   uint32_t dst_ip, uint16_t dst_port,
                   const std::vector<uint8_t>& payload = {});

    // Node IPs used in pcap frames (loopback range)
    static constexpr uint32_t IP_UE    = 0x7F000001; // 127.0.0.1
    static constexpr uint32_t IP_ENB   = 0x7F000002;
    static constexpr uint32_t IP_MME   = 0x7F000003;
    static constexpr uint32_t IP_HSS   = 0x7F000004;
    static constexpr uint32_t IP_SGW   = 0x7F000005;
    static constexpr uint32_t IP_PGW   = 0x7F000006;
    static constexpr uint32_t IP_PCRF  = 0x7F000007;
    static constexpr uint32_t IP_PCSCF = 0x7F000008;
    static constexpr uint32_t IP_SCSCF = 0x7F000009;
    static constexpr uint32_t IP_MTAS  = 0x7F00000A;

    static constexpr uint16_t PORT_S1AP  = 38412;  // our simulator uses 38412 (real 3GPP S1AP = 36412 on SCTP)
    static constexpr uint16_t PORT_DIA   = 3868;
    static constexpr uint16_t PORT_GX    = 3869;
    static constexpr uint16_t PORT_SGW   = 2123;
    static constexpr uint16_t PORT_PGW   = 2124;
    static constexpr uint16_t PORT_SIP   = 5060;

private:
    PcapWriter() = default;
    ~PcapWriter() { close(); }

    void writeGlobalHeader();
    void writePacket(const std::vector<uint8_t>& frame);

    // Proper TCP 3-way handshake with correct sequential seq numbers
    // Without this Wireshark shows "TCP Previous segment not captured"
    // and refuses to apply Diameter dissector even on port 3868
    void ensureTcpHandshake(uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port);

    // Build TCP frame with per-connection sequential seq numbers
    std::vector<uint8_t> buildIPTCP(uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint16_t dst_port,
                                    const std::vector<uint8_t>& payload,
                                    uint8_t tcp_flags = 0x18,
                                    uint32_t seq_num = 0,
                                    uint32_t ack_num = 0);
    std::vector<uint8_t> buildIPUDP(uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint16_t dst_port,
                                    const std::vector<uint8_t>& payload);

    // Per-connection seq number helpers
    bool     isClient   (uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port);
    uint32_t nextSeq    (uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port);
    uint32_t peerSeq    (uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port);
    void     advanceSeq (uint32_t src_ip, uint16_t src_port, uint32_t dst_ip, uint16_t dst_port, uint32_t bytes);

    static void     putU16  (std::vector<uint8_t>& v, uint16_t x);
    static void     putU32  (std::vector<uint8_t>& v, uint32_t x);
    static void     putU32le(std::vector<uint8_t>& v, uint32_t x);
    static uint16_t ipChecksum(const uint8_t* buf, int len);

    // Connection key: encode (src_ip, src_port, dst_ip, dst_port) as 96-bit value
    // We use a sorted pair so A→B and B→A share the same handshake
    static uint64_t connKey(uint32_t a_ip, uint16_t a_port,
                            uint32_t b_ip, uint16_t b_port) {
        // Sort so key is direction-independent
        uint64_t a = (uint64_t(a_ip) << 16) | a_port;
        uint64_t b = (uint64_t(b_ip) << 16) | b_port;
        return (a < b) ? (a << 32 | b) : (b << 32 | a);
    }

    // Per-connection seq tracker: connKey → {client_seq, server_seq}
    struct ConnSeq { uint32_t client_seq{1000}; uint32_t server_seq{2000}; };

    std::ofstream             file_;
    std::mutex                mtx_;
    std::atomic<uint32_t>     pkt_id_{1};          // just for IP ID field
    std::map<uint64_t, ConnSeq> conn_seq_;          // per-connection seq numbers
    std::set<uint64_t>          tcp_started_;       // connections that have SYN written
    bool                        open_{false};
};
