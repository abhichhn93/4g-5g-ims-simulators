#include "common/pcap_writer.h"
#include "common/logger.h"
#include <cstring>
#include <ctime>

// ── PCAP file format constants ────────────────────────────────
static constexpr uint32_t PCAP_MAGIC        = 0xa1b2c3d4; // little-endian
static constexpr uint16_t PCAP_MAJOR        = 2;
static constexpr uint16_t PCAP_MINOR        = 4;
static constexpr uint32_t PCAP_SNAPLEN      = 65535;
static constexpr uint32_t PCAP_LINKTYPE_RAW = 101; // raw IPv4 — no Ethernet

void PcapWriter::open(const std::string& filename) {
    std::lock_guard<std::mutex> lk(mtx_);
    file_.open(filename, std::ios::binary | std::ios::trunc);
    if (!file_) return;
    writeGlobalHeader();
    open_ = true;
    Logger::sys("PCAP: writing to " + filename +
                " — open in Wireshark after running simulator");
    Logger::sys("PCAP: Diameter packets → port 3868  │  GTPv2 → port 2123  │  SIP → port 5060");
}

void PcapWriter::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (open_) { file_.flush(); file_.close(); open_ = false; }
}

// ── Global header (24 bytes) ──────────────────────────────────
void PcapWriter::writeGlobalHeader() {
    std::vector<uint8_t> hdr;
    putU32le(hdr, PCAP_MAGIC);
    hdr.push_back(PCAP_MAJOR & 0xFF); hdr.push_back(PCAP_MAJOR >> 8);
    hdr.push_back(PCAP_MINOR & 0xFF); hdr.push_back(PCAP_MINOR >> 8);
    putU32le(hdr, 0);               // thiszone
    putU32le(hdr, 0);               // sigfigs
    putU32le(hdr, PCAP_SNAPLEN);
    putU32le(hdr, PCAP_LINKTYPE_RAW);
    file_.write(reinterpret_cast<const char*>(hdr.data()), hdr.size());
}

// ── Per-packet header (16 bytes) + payload ───────────────────
void PcapWriter::writePacket(const std::vector<uint8_t>& frame) {
    if (!open_) return;
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint32_t ts_sec  = uint32_t(std::chrono::duration_cast<std::chrono::seconds>(now).count());
    uint32_t ts_usec = uint32_t(std::chrono::duration_cast<std::chrono::microseconds>(now).count() % 1000000);
    uint32_t len     = uint32_t(frame.size());

    std::vector<uint8_t> pkt_hdr;
    putU32le(pkt_hdr, ts_sec);
    putU32le(pkt_hdr, ts_usec);
    putU32le(pkt_hdr, len);   // captured length
    putU32le(pkt_hdr, len);   // original length
    file_.write(reinterpret_cast<const char*>(pkt_hdr.data()), pkt_hdr.size());
    file_.write(reinterpret_cast<const char*>(frame.data()), frame.size());
    file_.flush();
}

// ── Diameter writer ───────────────────────────────────────────
// Diameter header: 20 bytes (RFC 6733 §3)
// Version(1) | Length(3) | Flags(1) | CmdCode(3) | AppID(4) | HbH(4) | E2E(4)
void PcapWriter::writeDiameter(DiameterCmd cmd, DiameterApp app,
                                bool is_request,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port,
                                const std::vector<uint8_t>& avp_data) {
    uint32_t code = static_cast<uint32_t>(cmd);
    uint32_t appid = static_cast<uint32_t>(app);
    uint32_t s = seq_.fetch_add(1);
    uint32_t total_len = 20 + uint32_t(avp_data.size());

    std::vector<uint8_t> dia;
    dia.push_back(1);                          // Version = 1
    dia.push_back((total_len >> 16) & 0xFF);   // Length high
    dia.push_back((total_len >>  8) & 0xFF);
    dia.push_back( total_len        & 0xFF);   // Length low
    dia.push_back(is_request ? 0x80 : 0x00);   // Flags: R=request
    dia.push_back((code >> 16) & 0xFF);        // Command Code (3B)
    dia.push_back((code >>  8) & 0xFF);
    dia.push_back( code        & 0xFF);
    putU32(dia, appid);                        // Application-ID
    putU32(dia, s);                            // Hop-by-Hop ID
    putU32(dia, s + 1000);                     // End-to-End ID

    // Add minimal Origin-Host AVP (264) so Wireshark shows more detail
    std::string origin = "mme.sim.local";
    uint32_t avp_len = 8 + uint32_t(origin.size());
    putU32(dia, 264);                          // AVP Code: Origin-Host
    dia.push_back(0x40);                       // Flags: Mandatory
    dia.push_back((avp_len >> 16) & 0xFF);
    dia.push_back((avp_len >>  8) & 0xFF);
    dia.push_back( avp_len        & 0xFF);
    for (char c : origin) dia.push_back(uint8_t(c));
    // Pad to 4-byte boundary
    while (dia.size() % 4) dia.push_back(0);

    // Append caller-supplied AVP data (our TLV payload)
    dia.insert(dia.end(), avp_data.begin(), avp_data.end());

    // Fix length after adding AVPs
    uint32_t real_len = uint32_t(dia.size());
    dia[1] = (real_len >> 16) & 0xFF;
    dia[2] = (real_len >>  8) & 0xFF;
    dia[3] =  real_len        & 0xFF;

    auto frame = buildIPTCP(src_ip, src_port, dst_ip, dst_port, dia);
    std::lock_guard<std::mutex> lk(mtx_);
    writePacket(frame);
}

// ── GTPv2 writer ──────────────────────────────────────────────
// GTPv2 header: 8 bytes (TS 29.274 §5.1)
// Flags(1) | MsgType(1) | Length(2) | TEID(4) | SeqNo(3) | Spare(1)
void PcapWriter::writeGTPv2(GtpMsgType msg_type, uint32_t teid,
                             uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port,
                             const std::vector<uint8_t>& ie_data) {
    std::vector<uint8_t> gtp;
    uint32_t s = seq_.fetch_add(1);
    uint16_t gtp_len = uint16_t(4 + ie_data.size()); // length excludes first 4 bytes

    gtp.push_back(0x48);                    // Flags: version=2, TEID present, seq present
    gtp.push_back(uint8_t(msg_type));       // Message Type
    gtp.push_back((gtp_len >> 8) & 0xFF);  // Length high
    gtp.push_back( gtp_len       & 0xFF);  // Length low
    putU32(gtp, teid);                      // TEID
    gtp.push_back((s >> 16) & 0xFF);       // Sequence Number (3B)
    gtp.push_back((s >>  8) & 0xFF);
    gtp.push_back( s        & 0xFF);
    gtp.push_back(0x00);                    // Spare
    gtp.insert(gtp.end(), ie_data.begin(), ie_data.end());

    auto frame = buildIPUDP(src_ip, src_port, dst_ip, dst_port, gtp);
    std::lock_guard<std::mutex> lk(mtx_);
    writePacket(frame);
}

// ── SIP writer ────────────────────────────────────────────────
void PcapWriter::writeSIP(const std::string& sip_text,
                           uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port) {
    std::vector<uint8_t> payload(sip_text.begin(), sip_text.end());
    auto frame = buildIPTCP(src_ip, src_port, dst_ip, dst_port, payload);
    std::lock_guard<std::mutex> lk(mtx_);
    writePacket(frame);
}

// ── IP + TCP frame builder ────────────────────────────────────
std::vector<uint8_t> PcapWriter::buildIPTCP(uint32_t src_ip, uint16_t src_port,
                                             uint32_t dst_ip, uint16_t dst_port,
                                             const std::vector<uint8_t>& payload) {
    // IP header (20 bytes, no options)
    uint16_t ip_total = uint16_t(20 + 20 + payload.size());
    std::vector<uint8_t> ip;
    ip.push_back(0x45);             // Version=4, IHL=5
    ip.push_back(0x00);             // DSCP/ECN
    putU16(ip, ip_total);           // Total Length
    putU16(ip, uint16_t(seq_.load())); // ID
    putU16(ip, 0x4000);             // Flags: Don't Fragment
    ip.push_back(64);               // TTL
    ip.push_back(6);                // Protocol: TCP
    putU16(ip, 0x0000);             // Checksum (filled below)
    putU32(ip, src_ip);
    putU32(ip, dst_ip);
    uint16_t cksum = ipChecksum(ip.data(), 20);
    ip[10] = (cksum >> 8) & 0xFF;
    ip[11] =  cksum       & 0xFF;

    // TCP header (20 bytes, no options, PSH+ACK flags)
    std::vector<uint8_t> tcp;
    putU16(tcp, src_port);
    putU16(tcp, dst_port);
    putU32(tcp, seq_.fetch_add(1)); // Sequence number
    putU32(tcp, 0);                 // Ack number
    tcp.push_back(0x50);            // Data offset = 5 (20 bytes), reserved = 0
    tcp.push_back(0x18);            // Flags: PSH + ACK
    putU16(tcp, 65535);             // Window size
    putU16(tcp, 0x0000);            // Checksum (0 = ignore)
    putU16(tcp, 0x0000);            // Urgent pointer

    std::vector<uint8_t> frame;
    frame.insert(frame.end(), ip.begin(),      ip.end());
    frame.insert(frame.end(), tcp.begin(),     tcp.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// ── IP + UDP frame builder ────────────────────────────────────
std::vector<uint8_t> PcapWriter::buildIPUDP(uint32_t src_ip, uint16_t src_port,
                                             uint32_t dst_ip, uint16_t dst_port,
                                             const std::vector<uint8_t>& payload) {
    uint16_t udp_len  = uint16_t(8 + payload.size());
    uint16_t ip_total = uint16_t(20 + udp_len);

    std::vector<uint8_t> ip;
    ip.push_back(0x45); ip.push_back(0x00);
    putU16(ip, ip_total);
    putU16(ip, uint16_t(seq_.load()));
    putU16(ip, 0x4000);
    ip.push_back(64); ip.push_back(17); // Protocol: UDP
    putU16(ip, 0x0000);
    putU32(ip, src_ip); putU32(ip, dst_ip);
    uint16_t cksum = ipChecksum(ip.data(), 20);
    ip[10] = (cksum >> 8) & 0xFF; ip[11] = cksum & 0xFF;

    std::vector<uint8_t> udp;
    putU16(udp, src_port); putU16(udp, dst_port);
    putU16(udp, udp_len);
    putU16(udp, 0x0000); // checksum (disabled)

    std::vector<uint8_t> frame;
    frame.insert(frame.end(), ip.begin(),      ip.end());
    frame.insert(frame.end(), udp.begin(),     udp.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

// ── Helpers ───────────────────────────────────────────────────
void PcapWriter::putU8 (std::vector<uint8_t>& v, uint8_t  x) { v.push_back(x); }
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
