#include "common/pcap_writer.h"
#include "common/logger.h"
#include <cstring>
#include <chrono>
#include <ctime>

static constexpr uint32_t PCAP_MAGIC        = 0xa1b2c3d4;
static constexpr uint16_t PCAP_MAJOR        = 2;
static constexpr uint16_t PCAP_MINOR        = 4;
static constexpr uint32_t PCAP_SNAPLEN      = 65535;
static constexpr uint32_t PCAP_LINKTYPE_ETH = 1;   // Ethernet

void PcapWriter::open(const std::string& filename) {
    std::lock_guard<std::mutex> lk(mtx_);
    file_.open(filename, std::ios::binary | std::ios::trunc);
    if (!file_) return;
    writeGlobalHeader();
    open_ = true;
    tcp_started_.clear();
    conn_seq_.clear();
    sctp_seq_.clear();
    Logger::sys("PCAP: writing to " + filename);
    Logger::sys("PCAP: Protocol Mapping Enabled:");
    Logger::sys("  S1AP:   SCTP port 36412, PPID 18");
    Logger::sys("  DIA:    TCP port 3868");
    Logger::sys("  GTPv2:  UDP port 2123");
    Logger::sys("  SIP:    UDP port 5060");
}

void PcapWriter::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (open_) { 
        file_.flush(); 
        file_.close(); 
        open_ = false; 
        Logger::sys("PCAP: Session packet capture finalized.");
    }
}

void PcapWriter::writeGlobalHeader() {
    std::vector<uint8_t> hdr;
    putU32le(hdr, PCAP_MAGIC);
    hdr.push_back(PCAP_MAJOR & 0xFF); hdr.push_back(PCAP_MAJOR >> 8);
    hdr.push_back(PCAP_MINOR & 0xFF); hdr.push_back(PCAP_MINOR >> 8);
    putU32le(hdr, 0); putU32le(hdr, 0);
    putU32le(hdr, PCAP_SNAPLEN);
    putU32le(hdr, PCAP_LINKTYPE_ETH);
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

// ── Diameter AVP helpers ─────────────────────────────────────
// Wireshark's Diameter dissector treats a 20-byte header-only message
// (no AVPs) as incomplete and shows raw "Data"/"Continuation" instead of
// "cmd=... flags=... appl=...". A real Diameter message always carries at
// least Session-Id/Origin-Host/Origin-Realm/Destination-Realm, so we
// auto-generate those when the caller doesn't supply AVPs of its own.
namespace {
std::string diamIdentity(uint32_t ip) {
    switch (ip) {
        case PcapWriter::IP_MME:   return "mme.epc.mnc001.mcc001.3gppnetwork.org";
        case PcapWriter::IP_HSS:   return "hss.epc.mnc001.mcc001.3gppnetwork.org";
        case PcapWriter::IP_SGW:   return "sgw.epc.mnc001.mcc001.3gppnetwork.org";
        case PcapWriter::IP_PGW:   return "pgw.epc.mnc001.mcc001.3gppnetwork.org";
        case PcapWriter::IP_PCRF:  return "pcrf.epc.mnc001.mcc001.3gppnetwork.org";
        case PcapWriter::IP_PCSCF: return "pcscf.ims.mnc001.mcc001.3gppnetwork.org";
        case PcapWriter::IP_SCSCF: return "scscf.ims.mnc001.mcc001.3gppnetwork.org";
        case PcapWriter::IP_MTAS:  return "mtas.ims.mnc001.mcc001.3gppnetwork.org";
        default:                   return "node.epc.mnc001.mcc001.3gppnetwork.org";
    }
}
std::string diamRealm(uint32_t ip) {
    switch (ip) {
        case PcapWriter::IP_PCSCF:
        case PcapWriter::IP_SCSCF:
        case PcapWriter::IP_MTAS:  return "ims.mnc001.mcc001.3gppnetwork.org";
        default:                   return "epc.mnc001.mcc001.3gppnetwork.org";
    }
}
// AVP header (RFC 6733 §4.1): Code(4) + Flags(1) + Length(3, includes header+data, excludes padding)
void addAvpStr(std::vector<uint8_t>& v, uint32_t code, const std::string& s) {
    uint32_t len = 8 + uint32_t(s.size());
    v.push_back((code >> 24) & 0xFF); v.push_back((code >> 16) & 0xFF);
    v.push_back((code >>  8) & 0xFF); v.push_back( code        & 0xFF);
    v.push_back(0x40); // Flags: M (mandatory)
    v.push_back((len >> 16) & 0xFF); v.push_back((len >> 8) & 0xFF); v.push_back(len & 0xFF);
    v.insert(v.end(), s.begin(), s.end());
    while (v.size() % 4) v.push_back(0); // pad to 4-byte boundary
}
} // namespace

// ── Diameter ──────────────────────────────────────────────────
// Write in our TLV format so the Lua dissector can decode and
// label it as "Diameter AIR [TS 29.272]" etc.
// The Lua dissector is registered for ports 3868/3869 and reads
// our [4B-len][2B-msg_type][2B-flags][4B-seq][TLVs] format.
void PcapWriter::writeDiameter(DiameterCmd cmd, DiameterApp app, bool is_request,
                                uint32_t src_ip, uint16_t src_port,
                                uint32_t dst_ip, uint16_t dst_port,
                                const std::vector<uint8_t>& avp_data) {
    std::vector<uint8_t> avps = avp_data;
    if (avps.empty()) {
        uint32_t sid = pkt_id_.fetch_add(1);
        addAvpStr(avps, 263, diamIdentity(src_ip) + ";" + std::to_string(sid)); // Session-Id
        addAvpStr(avps, 264, diamIdentity(src_ip));                             // Origin-Host
        addAvpStr(avps, 296, diamRealm(src_ip));                                // Origin-Realm
        addAvpStr(avps, 283, diamRealm(dst_ip));                                // Destination-Realm
    }

    // Standard Diameter Header (RFC 6733) — 20 bytes
    std::vector<uint8_t> frame;
    frame.push_back(0x01); // Version

    uint32_t msg_len = 20 + static_cast<uint32_t>(avps.size());
    frame.push_back((msg_len >> 16) & 0xFF);
    frame.push_back((msg_len >> 8) & 0xFF);
    frame.push_back(msg_len & 0xFF);

    frame.push_back(is_request ? 0xC0 : 0x40); // Flags: R+P for Request, P for Answer

    uint32_t c = static_cast<uint32_t>(cmd);
    frame.push_back((c >> 16) & 0xFF);
    frame.push_back((c >> 8) & 0xFF);
    frame.push_back(c & 0xFF);

    putU32(frame, static_cast<uint32_t>(app));

    uint32_t id = pkt_id_.fetch_add(1);
    putU32(frame, id); // Hop-by-Hop Identifier
    putU32(frame, id); // End-to-End Identifier

    frame.insert(frame.end(), avps.begin(), avps.end());

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
    // Length = TEID (4) + Sequence Number + Spare (4) + IEs — excludes the first 4 header bytes
    uint16_t gtp_len = static_cast<uint16_t>(8 + ie_data.size());

    std::vector<uint8_t> gtp;
    gtp.push_back(0x48);                    // Flags: version=2, T=1, seq present
    gtp.push_back(static_cast<uint8_t>(msg_type));
    gtp.push_back((gtp_len >> 8) & 0xFF);
    gtp.push_back( gtp_len       & 0xFF);
    putU32(gtp, teid);
    gtp.push_back((s >> 16) & 0xFF);
    gtp.push_back((s >>  8) & 0xFF);
    gtp.push_back( s        & 0xFF);
    gtp.push_back(0x00);
    gtp.insert(gtp.end(), ie_data.begin(), ie_data.end());

    std::lock_guard<std::mutex> lk(mtx_);
    writePacket(buildIPUDP(src_ip, PORT_SGW, dst_ip, PORT_SGW, gtp));
}

// ── SIP ───────────────────────────────────────────────────────
void PcapWriter::writeSIP(const std::string& sip_text,
                           uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port) {
    std::vector<uint8_t> payload(sip_text.begin(), sip_text.end());
    std::lock_guard<std::mutex> lk(mtx_);
    ensureTcpHandshake(src_ip, PORT_SIP, dst_ip, PORT_SIP);
    writePacket(buildIPTCP(src_ip, PORT_SIP, dst_ip, PORT_SIP, payload, 0x18,
                            nextSeq(src_ip, PORT_SIP, dst_ip, PORT_SIP),
                            peerSeq(src_ip, PORT_SIP, dst_ip, PORT_SIP)));
    advanceSeq(src_ip, PORT_SIP, dst_ip, PORT_SIP, uint32_t(payload.size()));
}

void PcapWriter::writeS1AP(const std::string& msg_name,
                            uint32_t src_ip, uint16_t src_port,
                            uint32_t dst_ip, uint16_t dst_port,
                            const std::vector<uint8_t>& payload) {
    (void)msg_name; // kept for call-site readability; payload is real ASN.1 APER
    (void)src_port; (void)dst_port; // S1AP always uses SCTP port 36412
    std::lock_guard<std::mutex> lk(mtx_);
    // SCTP encapsulation with PPID 18 — Wireshark's native "s1ap" dissector
    // decodes the APER-encoded payload directly.
    writePacket(buildIPSCTP(src_ip, PORT_S1AP, dst_ip, PORT_S1AP, payload, 18));
}

std::vector<uint8_t> PcapWriter::buildEthernet() {
    std::vector<uint8_t> eth(14);
    std::memset(eth.data(), 0x02, 6);      // Dst MAC dummy
    std::memset(eth.data() + 6, 0x01, 6);  // Src MAC dummy
    eth[12] = 0x08; eth[13] = 0x00;        // EtherType = IPv4
    return eth;
}

std::vector<uint8_t> PcapWriter::buildIPSCTP(uint32_t src_ip, uint16_t src_port,
                                              uint32_t dst_ip, uint16_t dst_port,
                                              const std::vector<uint8_t>& payload,
                                              uint32_t ppid) {
    // SCTP DATA chunks are padded to a 4-byte boundary, and that padding is
    // part of the SCTP packet (hence part of the IP payload) — RFC 4960 §3.2.
    // chunk_len itself (set below) does NOT include this padding.
    size_t chunk_pad = (4 - ((16 + payload.size()) % 4)) % 4;

    // IPv4 Header (20 bytes)
    uint16_t ip_total = uint16_t(20 + 12 + 16 + payload.size() + chunk_pad);
    std::vector<uint8_t> ip;
    ip.push_back(0x45); ip.push_back(0x00);
    putU16(ip, ip_total);
    putU16(ip, uint16_t(pkt_id_.fetch_add(1)));
    putU16(ip, 0x4000); // DF flag
    ip.push_back(64); ip.push_back(132); // protocol = SCTP (132)
    putU16(ip, 0x0000);
    putU32(ip, src_ip); putU32(ip, dst_ip);
    uint16_t ck = ipChecksum(ip.data(), 20);
    ip[10] = (ck >> 8) & 0xFF; ip[11] = ck & 0xFF;

    // SCTP Common Header (12 bytes)
    std::vector<uint8_t> sctp;
    putU16(sctp, src_port);
    putU16(sctp, dst_port);
    putU32(sctp, 0x12345678); // Verification Tag
    putU32(sctp, 0x00000000); // Checksum dummy

    // SCTP DATA Chunk Header (16 bytes)
    // TSN/Stream-Seq must increment (and never repeat across either
    // direction, since both directions share one verification tag), or
    // Wireshark treats the packet as a "retransmission" and skips the
    // PPID sub-dissector.
    auto sctp_key = connKey(src_ip, src_port, dst_ip, dst_port);
    auto& ss = sctp_seq_[sctp_key];
    uint32_t tsn = ss.tsn++;
    uint16_t ssn = ss.ssn++;

    std::vector<uint8_t> chunk;
    chunk.push_back(0x00); // Type = DATA
    chunk.push_back(0x03); // Flags (E=1, B=1)
    uint16_t chunk_len = uint16_t(16 + payload.size());
    putU16(chunk, chunk_len);
    putU32(chunk, tsn); // TSN
    putU16(chunk, 0);   // Stream ID
    putU16(chunk, ssn); // Stream Seq
    putU32(chunk, ppid); // PPID (18 for S1AP)

    std::vector<uint8_t> frame = buildEthernet();
    frame.insert(frame.end(), ip.begin(), ip.end());
    frame.insert(frame.end(), sctp.begin(), sctp.end());
    frame.insert(frame.end(), chunk.begin(), chunk.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    frame.insert(frame.end(), chunk_pad, 0); // SCTP chunk padding
    return frame;
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

    // Fix: Include Ethernet header for LINKTYPE_ETH compatibility
    std::vector<uint8_t> frame = buildEthernet();
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
    putU16(ip, uint16_t(pkt_id_.fetch_add(1)));
    putU16(ip, 0x4000);
    ip.push_back(64); ip.push_back(17); // UDP
    putU16(ip, 0x0000);
    putU32(ip, src_ip); putU32(ip, dst_ip);
    uint16_t ck = ipChecksum(ip.data(), 20);
    ip[10] = (ck >> 8) & 0xFF; ip[11] = ck & 0xFF;

    std::vector<uint8_t> udp;
    putU16(udp, src_port); putU16(udp, dst_port);
    putU16(udp, udp_len);  putU16(udp, 0);

    // Fix: Include Ethernet header for LINKTYPE_ETH compatibility
    std::vector<uint8_t> frame = buildEthernet();
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
