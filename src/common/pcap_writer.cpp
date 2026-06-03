#include "common/pcap_writer.h"
#include "common/logger.h"
#include "common/tlv.h"
#include "common/message_types.h"
#include <cstring>
#include <ctime>

static constexpr uint32_t PCAP_MAGIC        = 0xa1b2c3d4;
static constexpr uint16_t PCAP_MAJOR        = 2;
static constexpr uint16_t PCAP_MINOR        = 4;
static constexpr uint32_t PCAP_SNAPLEN      = 65535;
static constexpr uint32_t PCAP_LINKTYPE_RAW = 101; // raw IPv4

void PcapWriter::open(const std::string& filename) {
    std::lock_guard<std::mutex> lk(mtx_);
    file_.open(filename, std::ios::binary | std::ios::trunc);
    if (!file_) return;
    writeGlobalHeader();
    open_ = true;
    tcp_started_.clear();
    conn_seq_.clear();
    Logger::sys("PCAP: writing to " + filename);
    Logger::sys("PCAP: Wireshark filters:");
    Logger::sys("  diameter              → Diameter S6a (MME↔HSS) + Gx (P-GW↔PCRF)");
    Logger::sys("  gtpv2                 → GTPv2 S11 (MME↔S-GW) + S5 (S-GW↔P-GW)");
    Logger::sys("  sip                   → SIP IMS (UE↔P-CSCF↔S-CSCF)");
    Logger::sys("  tcp.port==36412       → S1AP (eNB↔MME) with Lua dissector");
    Logger::sys("  diameter || gtpv2 || sip || tcp.port==36412  → all nodes");
}

void PcapWriter::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (open_) { file_.flush(); file_.close(); open_ = false; }
}

void PcapWriter::writeGlobalHeader() {
    std::vector<uint8_t> hdr;
    putU32le(hdr, PCAP_MAGIC);
    hdr.push_back(PCAP_MAJOR & 0xFF); hdr.push_back(PCAP_MAJOR >> 8);
    hdr.push_back(PCAP_MINOR & 0xFF); hdr.push_back(PCAP_MINOR >> 8);
    putU32le(hdr, 0); putU32le(hdr, 0);
    putU32le(hdr, PCAP_SNAPLEN);
    putU32le(hdr, PCAP_LINKTYPE_RAW);
    file_.write(reinterpret_cast<const char*>(hdr.data()), hdr.size());
}

void PcapWriter::writePacket(const std::vector<uint8_t>& frame) {
    if (!open_) return;
    auto now    = std::chrono::system_clock::now().time_since_epoch();
    uint32_t ts_sec  = uint32_t(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    uint32_t ts_usec = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now).count() % 1000000);
    uint32_t len     = uint32_t(frame.size());
    std::vector<uint8_t> pkt;
    putU32le(pkt, ts_sec); putU32le(pkt, ts_usec);
    putU32le(pkt, len);    putU32le(pkt, len);
    file_.write(reinterpret_cast<const char*>(pkt.data()),   pkt.size());
    file_.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    file_.flush();
}

// ── TCP 3-way handshake with CORRECT sequential seq numbers ──
//
// WHY this matters: Wireshark tracks TCP streams by (src,dst,sport,dport).
// If seq numbers are random/wrong, it shows "TCP Previous segment not
// captured" and refuses to apply the Diameter dissector even on port 3868.
//
// Correct sequence:
//   SYN     client→server   seq=ISN_C  ack=0
//   SYN-ACK server→client   seq=ISN_S  ack=ISN_C+1
//   ACK     client→server   seq=ISN_C+1 ack=ISN_S+1
//   DATA    client→server   seq=ISN_C+1 ack=ISN_S+1
//
// We use ISN_C=1000 and ISN_S=2000 per connection (simple, valid).
void PcapWriter::ensureTcpHandshake(uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port) {
    auto key = connKey(src_ip, src_port, dst_ip, dst_port);
    if (tcp_started_.count(key)) return;
    tcp_started_.insert(key);

    auto& cs = conn_seq_[key];
    cs.client_seq = 1000;
    cs.server_seq = 2000;

    std::vector<uint8_t> empty;
    // SYN:     client seq=1000, ack=0
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, empty, 0x02, cs.client_seq, 0));
    // SYN-ACK: server seq=2000, ack=1001
    writePacket(buildIPTCP(dst_ip, dst_port, src_ip, src_port, empty, 0x12, cs.server_seq, cs.client_seq + 1));
    // ACK:     client seq=1001, ack=2001
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, empty, 0x10, cs.client_seq + 1, cs.server_seq + 1));
    // Advance client seq past handshake
    cs.client_seq += 1;  // now 1001
    cs.server_seq += 1;  // now 2001
}

// ── Diameter ──────────────────────────────────────────────────
// Write in our TLV format so the Lua dissector can decode and
// label it as "Diameter AIR [TS 29.272]" etc.
// The Lua dissector is registered for ports 3868/3869 and reads
// our [4B-len][2B-msg_type][2B-flags][4B-seq][TLVs] format.
void PcapWriter::writeDiameter(DiameterCmd cmd, DiameterApp app, bool is_request,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port,
                                const std::vector<uint8_t>& avp_data) {
    // Map to our internal MessageType that Lua dissector knows
    uint16_t mtype_val = 0xFFFF;
    if (cmd == DiameterCmd::AUTH_INFO) {
        mtype_val = is_request ? 0x0101 : 0x0102;  // AIR / AIA
    } else if (cmd == DiameterCmd::UPDATE_LOCATION) {
        mtype_val = is_request ? 0x0103 : 0x0104;  // ULR / ULA
    } else if (app == DiameterApp::GX) {
        mtype_val = is_request ? 0x0401 : 0x0402;  // Gx CCR / CCA
    } else if (app == DiameterApp::CX || cmd == DiameterCmd::SERVER_ASSIGNMENT) {
        mtype_val = is_request ? 0x0501 : 0x0502;  // Cx SAR / SAA
    }

    // Build our TLV frame — Lua dissector reads this natively
    MessageWriter w(static_cast<MessageType>(mtype_val), pkt_id_.fetch_add(1));
    // Optionally embed some key IEs from avp_data
    if (!avp_data.empty())
        w.writeBytes(static_cast<Tag>(0x0200), avp_data.data(),
                     uint16_t(std::min(avp_data.size(), size_t(8))));

    auto frame = w.frame();
    std::lock_guard<std::mutex> lk(mtx_);
    ensureTcpHandshake(src_ip, src_port, dst_ip, dst_port);
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, frame, 0x18,
                            nextSeq(src_ip, src_port, dst_ip, dst_port),
                            peerSeq(src_ip, src_port, dst_ip, dst_port)));
    advanceSeq(src_ip, src_port, dst_ip, dst_port, uint32_t(frame.size()));
}

// ── GTPv2 ─────────────────────────────────────────────────────
void PcapWriter::writeGTPv2(GtpMsgType msg_type, uint32_t teid,
                             uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port,
                             const std::vector<uint8_t>& ie_data) {
    uint32_t s = pkt_id_.fetch_add(1);
    uint16_t gtp_len = uint16_t(4 + ie_data.size());

    std::vector<uint8_t> gtp;
    gtp.push_back(0x48);                    // Flags: version=2, T=1, seq present
    gtp.push_back(uint8_t(msg_type));
    gtp.push_back((gtp_len >> 8) & 0xFF);
    gtp.push_back( gtp_len       & 0xFF);
    putU32(gtp, teid);
    gtp.push_back((s >> 16) & 0xFF);
    gtp.push_back((s >>  8) & 0xFF);
    gtp.push_back( s        & 0xFF);
    gtp.push_back(0x00);
    gtp.insert(gtp.end(), ie_data.begin(), ie_data.end());

    std::lock_guard<std::mutex> lk(mtx_);
    writePacket(buildIPUDP(src_ip, src_port, dst_ip, dst_port, gtp));
}

// ── SIP ───────────────────────────────────────────────────────
void PcapWriter::writeSIP(const std::string& sip_text,
                           uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port) {
    std::vector<uint8_t> payload(sip_text.begin(), sip_text.end());
    std::lock_guard<std::mutex> lk(mtx_);
    ensureTcpHandshake(src_ip, src_port, dst_ip, dst_port);
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, payload, 0x18,
                            nextSeq(src_ip, src_port, dst_ip, dst_port),
                            peerSeq(src_ip, src_port, dst_ip, dst_port)));
    advanceSeq(src_ip, src_port, dst_ip, dst_port, uint32_t(payload.size()));
}

// ── S1AP — write our TLV so Lua dissector shows proper label ──
void PcapWriter::writeS1AP(const std::string& msg_name,
                            uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port,
                            const std::vector<uint8_t>& payload) {
    // Map message name to our MessageType value (Lua dissector knows these)
    uint16_t mtype_val = 0xFFFF;
    if      (msg_name.find("InitialUEMsg")       != std::string::npos) mtype_val = 0x0001;
    else if (msg_name.find("AuthRequest")        != std::string::npos ||
             msg_name.find("AuthReq")            != std::string::npos) mtype_val = 0x0002;
    else if (msg_name.find("SecurityModeCmd")    != std::string::npos) mtype_val = 0x0006;
    else if (msg_name.find("SecurityModeComp")   != std::string::npos ||
             msg_name.find("SecurityModeComplete")!= std::string::npos) mtype_val = 0x0007;
    else if (msg_name.find("AuthResponse")       != std::string::npos ||
             msg_name.find("AuthRsp")            != std::string::npos) mtype_val = 0x0003;
    else if (msg_name.find("AttachComplete")     != std::string::npos) mtype_val = 0x0003;
    else if (msg_name.find("InitialContextSetupReq") != std::string::npos) mtype_val = 0x0004;
    else if (msg_name.find("InitialContextSetupRsp") != std::string::npos) mtype_val = 0x0005;

    // If we got the original TLV payload, use it directly (already has correct msg_type)
    std::vector<uint8_t> frame;
    if (!payload.empty()) {
        frame = payload;  // already our TLV format with 4B length prefix
    } else {
        // Build minimal TLV frame for Lua dissector
        MessageWriter w(static_cast<MessageType>(mtype_val), pkt_id_.fetch_add(1));
        frame = w.frame();
    }

    std::lock_guard<std::mutex> lk(mtx_);
    ensureTcpHandshake(src_ip, src_port, dst_ip, dst_port);
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, frame, 0x18,
                            nextSeq(src_ip, src_port, dst_ip, dst_port),
                            peerSeq(src_ip, src_port, dst_ip, dst_port)));
    advanceSeq(src_ip, src_port, dst_ip, dst_port, uint32_t(frame.size()));
}

// ── Per-connection seq helpers ─────────────────────────────────
// These determine which seq belongs to which direction.
// "client" = lower connKey half, "server" = higher.
bool PcapWriter::isClient(uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port) {
    uint64_t a = (uint64_t(src_ip) << 16) | src_port;
    uint64_t b = (uint64_t(dst_ip) << 16) | dst_port;
    return a <= b;
}

uint32_t PcapWriter::nextSeq(uint32_t src_ip, uint16_t src_port,
                              uint32_t dst_ip, uint16_t dst_port) {
    auto key = connKey(src_ip, src_port, dst_ip, dst_port);
    auto& cs = conn_seq_[key];
    return isClient(src_ip, src_port, dst_ip, dst_port) ? cs.client_seq : cs.server_seq;
}

uint32_t PcapWriter::peerSeq(uint32_t src_ip, uint16_t src_port,
                              uint32_t dst_ip, uint16_t dst_port) {
    auto key = connKey(src_ip, src_port, dst_ip, dst_port);
    auto& cs = conn_seq_[key];
    return isClient(src_ip, src_port, dst_ip, dst_port) ? cs.server_seq : cs.client_seq;
}

void PcapWriter::advanceSeq(uint32_t src_ip, uint16_t src_port,
                              uint32_t dst_ip, uint16_t dst_port, uint32_t bytes) {
    auto key = connKey(src_ip, src_port, dst_ip, dst_port);
    auto& cs = conn_seq_[key];
    if (isClient(src_ip, src_port, dst_ip, dst_port)) cs.client_seq += bytes;
    else cs.server_seq += bytes;
}

// ── IP + TCP frame ─────────────────────────────────────────────
std::vector<uint8_t> PcapWriter::buildIPTCP(uint32_t src_ip, uint16_t src_port,
                                             uint32_t dst_ip, uint16_t dst_port,
                                             const std::vector<uint8_t>& payload,
                                             uint8_t tcp_flags,
                                             uint32_t seq_num,
                                             uint32_t ack_num) {
    uint16_t ip_total = uint16_t(20 + 20 + payload.size());
    std::vector<uint8_t> ip;
    ip.push_back(0x45); ip.push_back(0x00);
    putU16(ip, ip_total);
    putU16(ip, uint16_t(pkt_id_.fetch_add(1)));
    putU16(ip, 0x4000);
    ip.push_back(64); ip.push_back(6); // protocol = TCP
    putU16(ip, 0x0000);
    putU32(ip, src_ip); putU32(ip, dst_ip);
    uint16_t ck = ipChecksum(ip.data(), 20);
    ip[10] = (ck >> 8) & 0xFF; ip[11] = ck & 0xFF;

    // ACK flag: set for all except SYN
    bool has_ack = (tcp_flags != 0x02);

    std::vector<uint8_t> tcp;
    putU16(tcp, src_port);
    putU16(tcp, dst_port);
    putU32(tcp, seq_num);
    putU32(tcp, has_ack ? ack_num : 0);
    tcp.push_back(0x50);       // data offset = 5 (20 bytes, no options)
    tcp.push_back(tcp_flags);
    putU16(tcp, 65535);        // window size
    putU16(tcp, 0x0000);       // checksum (0 = disabled, Wireshark ignores)
    putU16(tcp, 0x0000);       // urgent pointer

    std::vector<uint8_t> frame;
    frame.insert(frame.end(), ip.begin(),      ip.end());
    frame.insert(frame.end(), tcp.begin(),     tcp.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// ── IP + UDP frame ─────────────────────────────────────────────
std::vector<uint8_t> PcapWriter::buildIPUDP(uint32_t src_ip, uint16_t src_port,
                                             uint32_t dst_ip, uint16_t dst_port,
                                             const std::vector<uint8_t>& payload) {
    uint16_t udp_len  = uint16_t(8 + payload.size());
    uint16_t ip_total = uint16_t(20 + udp_len);
    std::vector<uint8_t> ip;
    ip.push_back(0x45); ip.push_back(0x00);
    putU16(ip, ip_total);
    putU16(ip, uint16_t(pkt_id_.load()));
    putU16(ip, 0x4000);
    ip.push_back(64); ip.push_back(17); // UDP
    putU16(ip, 0x0000);
    putU32(ip, src_ip); putU32(ip, dst_ip);
    uint16_t ck = ipChecksum(ip.data(), 20);
    ip[10] = (ck >> 8) & 0xFF; ip[11] = ck & 0xFF;

    std::vector<uint8_t> udp;
    putU16(udp, src_port); putU16(udp, dst_port);
    putU16(udp, udp_len);  putU16(udp, 0);

    std::vector<uint8_t> frame;
    frame.insert(frame.end(), ip.begin(),      ip.end());
    frame.insert(frame.end(), udp.begin(),     udp.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

void PcapWriter::putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF);
}
void PcapWriter::putU32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((x >> 24) & 0xFF); v.push_back((x >> 16) & 0xFF);
    v.push_back((x >>  8) & 0xFF); v.push_back( x        & 0xFF);
}
void PcapWriter::putU32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back( x        & 0xFF); v.push_back((x >>  8) & 0xFF);
    v.push_back((x >> 16) & 0xFF); v.push_back((x >> 24) & 0xFF);
}
uint16_t PcapWriter::ipChecksum(const uint8_t* buf, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2)
        sum += (uint32_t(buf[i]) << 8) | buf[i+1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return uint16_t(~sum);
}
