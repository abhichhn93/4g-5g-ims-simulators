#pragma once
// ============================================================
// PCAP WRITER — writes real protocol headers so Wireshark
// shows Diameter / GTPv2 / SIP (not just raw TCP/UDP)
//
// APPROACH:
//   We keep TCP/UDP as transport (easy, cross-platform).
//   We write real APPLICATION-LAYER headers on top:
//     - Diameter (RFC 6733): 20-byte header → Wireshark shows "Diameter"
//     - GTPv2    (TS 29.274): 8-byte header → Wireshark shows "GTPv2"
//     - SIP      (RFC 3261): text format → Wireshark shows "SIP"
//
//   Port numbers tell Wireshark which dissector to use:
//     3868 → Diameter   2123/2124 → GTPv2   5060 → SIP
//
// FORMAT: PCAP with LINKTYPE_RAW (101) = raw IPv4
//   Each packet: pcap_pkthdr (16B) + IP header (20B) + TCP/UDP + payload
//
// INTERVIEW: "Why not real SCTP?"
//   macOS doesn't support SCTP in userspace without kernel extensions.
//   Wireshark identifies protocols by port + application header, not
//   transport. TCP on port 3868 → Diameter decode. Perfectly valid.
// ============================================================
#include <cstdint>
#include <string>
#include <mutex>
#include <fstream>
#include <vector>
#include <atomic>
#include <chrono>
#include <cstring>

// ── Diameter Command Codes (TS 29.272, TS 29.212) ────────────
enum class DiameterCmd : uint32_t {
    CREDIT_CONTROL    = 272,   // Gx CCR/CCA
    AUTH_INFO         = 318,   // S6a AIR/AIA
    UPDATE_LOCATION   = 316,   // S6a ULR/ULA
    SERVER_ASSIGNMENT = 301,   // Cx SAR/SAA
    USER_AUTH         = 300,   // Cx UAR/UAA
};

enum class DiameterApp : uint32_t {
    BASE         = 0,
    S6A          = 16777251,  // 3GPP S6a (TS 29.272)
    GX           = 16777238,  // 3GPP Gx  (TS 29.212)
    CX           = 16777216,  // 3GPP Cx  (TS 29.229)
};

// ── GTPv2 Message Types (TS 29.274 §8.1) ─────────────────────
enum class GtpMsgType : uint8_t {
    CREATE_SESSION_REQ  = 32,
    CREATE_SESSION_RSP  = 33,
    MODIFY_BEARER_REQ   = 34,
    MODIFY_BEARER_RSP   = 35,
    DELETE_SESSION_REQ  = 36,
    DELETE_SESSION_RSP  = 37,
};

class PcapWriter {
public:
    static PcapWriter& instance() { static PcapWriter p; return p; }

    void open(const std::string& filename = "mme_capture.pcap");
    void close();

    // Write a Diameter message (shows as "Diameter" in Wireshark)
    void writeDiameter(DiameterCmd cmd, DiameterApp app,
                       bool is_request,
                       uint32_t src_ip, uint16_t src_port,
                       uint32_t dst_ip, uint16_t dst_port,
                       const std::vector<uint8_t>& avp_data = {});

    // Write a GTPv2 message (shows as "GTPv2" in Wireshark)
    void writeGTPv2(GtpMsgType msg_type,
                    uint32_t teid,
                    uint32_t src_ip, uint16_t src_port,
                    uint32_t dst_ip, uint16_t dst_port,
                    const std::vector<uint8_t>& ie_data = {});

    // Write a SIP message (shows as "SIP" in Wireshark)
    void writeSIP(const std::string& sip_text,
                  uint32_t src_ip, uint16_t src_port,
                  uint32_t dst_ip, uint16_t dst_port);

    // Convenience IPs
    static constexpr uint32_t IP_UE   = 0x7F000001; // 127.0.0.1
    static constexpr uint32_t IP_ENB  = 0x7F000002;
    static constexpr uint32_t IP_MME  = 0x7F000003;
    static constexpr uint32_t IP_HSS  = 0x7F000004;
    static constexpr uint32_t IP_SGW  = 0x7F000005;
    static constexpr uint32_t IP_PGW  = 0x7F000006;
    static constexpr uint32_t IP_PCRF = 0x7F000007;
    static constexpr uint32_t IP_PCSCF= 0x7F000008;
    static constexpr uint32_t IP_SCSCF= 0x7F000009;

private:
    PcapWriter() = default;
    ~PcapWriter() { close(); }

    void writeGlobalHeader();
    void writePacket(const std::vector<uint8_t>& frame);

    std::vector<uint8_t> buildIPTCP(uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint16_t dst_port,
                                    const std::vector<uint8_t>& payload);
    std::vector<uint8_t> buildIPUDP(uint32_t src_ip, uint16_t src_port,
                                    uint32_t dst_ip, uint16_t dst_port,
                                    const std::vector<uint8_t>& payload);

    static void putU8 (std::vector<uint8_t>& v, uint8_t  x);
    static void putU16(std::vector<uint8_t>& v, uint16_t x); // big-endian
    static void putU32(std::vector<uint8_t>& v, uint32_t x); // big-endian
    static void putU32le(std::vector<uint8_t>& v, uint32_t x); // little-endian
    static uint16_t ipChecksum(const uint8_t* buf, int len);

    std::ofstream     file_;
    std::mutex        mtx_;
    std::atomic<uint32_t> seq_{1};
    bool              open_{false};
};
