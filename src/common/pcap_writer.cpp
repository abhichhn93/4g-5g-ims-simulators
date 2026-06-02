#include "common/pcap_writer.h"
#include "common/logger.h"
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
void PcapWriter::writeDiameter(DiameterCmd cmd, DiameterApp app, bool is_request,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port,
                                const std::vector<uint8_t>& avp_data) {
    uint32_t code  = static_cast<uint32_t>(cmd);
    uint32_t appid = static_cast<uint32_t>(app);
    uint32_t s     = pkt_id_.fetch_add(1);

    // Diameter header (20 bytes, RFC 6733 §3)
    std::vector<uint8_t> dia;
    dia.push_back(1);                         // Version = 1
    dia.push_back(0); dia.push_back(0); dia.push_back(0); // Length placeholder
    dia.push_back(is_request ? 0x80 : 0x00);  // Flags: R-bit
    dia.push_back((code >> 16) & 0xFF);       // Command Code (3 bytes)
    dia.push_back((code >>  8) & 0xFF);
    dia.push_back( code        & 0xFF);
    putU32(dia, appid);                        // Application-ID
    putU32(dia, s);                            // Hop-by-Hop ID
    putU32(dia, s + 0x1000);                   // End-to-End ID

    // Origin-Host AVP (264) — makes Wireshark show NF names
    std::string origin = is_request ? "mme.sim.local" : "hss.sim.local";
    if (app == DiameterApp::GX)
        origin = is_request ? "pgw.sim.local" : "pcrf.sim.local";
    // AVP: Code(4) + Flags(1) + Length(3) + Data
    uint32_t avp_total = 8 + uint32_t(origin.size());
    putU32(dia, 264);                          // AVP Code: Origin-Host
    dia.push_back(0x40);                       // Mandatory flag
    dia.push_back((avp_total >> 16) & 0xFF);
    dia.push_back((avp_total >>  8) & 0xFF);
    dia.push_back( avp_total        & 0xFF);
    for (char c : origin) dia.push_back(uint8_t(c));
    while (dia.size() % 4) dia.push_back(0);  // 4-byte padding

    // Session-Id AVP (263) — session correlation
    std::string session = "session." + std::to_string(s);
    uint32_t sid_len = 8 + uint32_t(session.size());
    putU32(dia, 263);
    dia.push_back(0x40);
    dia.push_back((sid_len >> 16) & 0xFF);
    dia.push_back((sid_len >>  8) & 0xFF);
    dia.push_back( sid_len        & 0xFF);
    for (char c : session) dia.push_back(uint8_t(c));
    while (dia.size() % 4) dia.push_back(0);

    // Auth-Application-Id AVP (258)
    putU32(dia, 258); dia.push_back(0x40);
    dia.push_back(0); dia.push_back(0); dia.push_back(12); // length=12
    putU32(dia, appid);

    dia.insert(dia.end(), avp_data.begin(), avp_data.end());

    // Fix length
    uint32_t total = uint32_t(dia.size());
    dia[1] = (total >> 16) & 0xFF;
    dia[2] = (total >>  8) & 0xFF;
    dia[3] =  total        & 0xFF;

    std::lock_guard<std::mutex> lk(mtx_);
    ensureTcpHandshake(src_ip, src_port, dst_ip, dst_port);
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, dia, 0x18,
                            nextSeq(src_ip, src_port, dst_ip, dst_port),
                            peerSeq(src_ip, src_port, dst_ip, dst_port)));
    advanceSeq(src_ip, src_port, dst_ip, dst_port, uint32_t(dia.size()));
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

// ── S1AP (our custom TLV — Lua dissector decodes it) ──────────
void PcapWriter::writeS1AP(const std::string& msg_name,
                            uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port,
                            const std::vector<uint8_t>& payload) {
    // Prefix with null-terminated message name so Lua dissector can show it
    std::vector<uint8_t> data;
    for (char c : msg_name) data.push_back(uint8_t(c));
    data.push_back(0);
    data.insert(data.end(), payload.begin(), payload.end());

    std::lock_guard<std::mutex> lk(mtx_);
    ensureTcpHandshake(src_ip, src_port, dst_ip, dst_port);
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, data, 0x18,
                            nextSeq(src_ip, src_port, dst_ip, dst_port),
                            peerSeq(src_ip, src_port, dst_ip, dst_port)));
    advanceSeq(src_ip, src_port, dst_ip, dst_port, uint32_t(data.size()));
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
