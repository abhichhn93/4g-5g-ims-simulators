#include "common/pcap_writer.h"
#include "common/logger.h"
#include <cstring>
#include <chrono>

static constexpr uint32_t PCAP_MAGIC        = 0xa1b2c3d4;
static constexpr uint16_t PCAP_MAJOR        = 2;
static constexpr uint16_t PCAP_MINOR        = 4;
static constexpr uint32_t PCAP_SNAPLEN      = 65535;
static constexpr uint32_t PCAP_LINKTYPE_ETH = 1; // Ethernet

void PcapWriter::open(const std::string& filename) {
    std::lock_guard<std::mutex> lk(mtx_);
    file_.open(filename, std::ios::binary | std::ios::trunc);
    if (!file_) return;
    writeGlobalHeader();
    open_ = true;
    tcp_started_.clear();
    conn_seq_.clear();
    Logger::sys("PCAP: writing to " + filename);
    Logger::sys("PCAP: N2 (gNB<->AMF) on TCP/38412, SBI (AMF<->UDM) on TCP/80 -> Wireshark 'HTTP'");
}

void PcapWriter::close() {
    std::lock_guard<std::mutex> lk(mtx_);
    if (open_) {
        file_.flush();
        file_.close();
        open_ = false;
        Logger::sys("PCAP: capture finalized.");
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

// ── TCP 3-way handshake with correct sequential seq numbers ──
// Without this Wireshark shows "TCP Previous segment not captured"
// and won't apply the HTTP dissector to later segments.
void PcapWriter::ensureTcpHandshake(uint32_t src_ip, uint16_t src_port,
                                     uint32_t dst_ip, uint16_t dst_port) {
    auto key = connKey(src_ip, src_port, dst_ip, dst_port);
    if (tcp_started_.count(key)) return;
    tcp_started_.insert(key);

    auto& cs = conn_seq_[key];
    cs.client_seq = 1000;
    cs.server_seq = 2000;

    std::vector<uint8_t> empty;
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, empty, 0x02, cs.client_seq, 0));
    writePacket(buildIPTCP(dst_ip, dst_port, src_ip, src_port, empty, 0x12, cs.server_seq, cs.client_seq + 1));
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, empty, 0x10, cs.client_seq + 1, cs.server_seq + 1));
    cs.client_seq += 1;
    cs.server_seq += 1;
}

void PcapWriter::writeAppText(const std::string& text,
                               uint32_t src_ip, uint16_t src_port,
                               uint32_t dst_ip, uint16_t dst_port) {
    std::vector<uint8_t> payload(text.begin(), text.end());
    std::lock_guard<std::mutex> lk(mtx_);
    ensureTcpHandshake(src_ip, src_port, dst_ip, dst_port);
    writePacket(buildIPTCP(src_ip, src_port, dst_ip, dst_port, payload, 0x18,
                            nextSeq(src_ip, src_port, dst_ip, dst_port),
                            peerSeq(src_ip, src_port, dst_ip, dst_port)));
    advanceSeq(src_ip, src_port, dst_ip, dst_port, uint32_t(payload.size()));
}

std::vector<uint8_t> PcapWriter::buildEthernet() {
    std::vector<uint8_t> eth(14);
    std::memset(eth.data(), 0x02, 6);     // Dst MAC dummy
    std::memset(eth.data() + 6, 0x01, 6); // Src MAC dummy
    eth[12] = 0x08; eth[13] = 0x00;       // EtherType = IPv4
    return eth;
}

bool PcapWriter::isClient(uint32_t src_ip, uint16_t src_port,
                           uint32_t dst_ip, uint16_t dst_port) {
    uint64_t a = (uint64_t(src_ip) << 16) | src_port;
    uint64_t b = (uint64_t(dst_ip) << 16) | dst_port;
    return a <= b;
}

uint32_t PcapWriter::nextSeq(uint32_t src_ip, uint16_t src_port,
                              uint32_t dst_ip, uint16_t dst_port) {
    auto& cs = conn_seq_[connKey(src_ip, src_port, dst_ip, dst_port)];
    return isClient(src_ip, src_port, dst_ip, dst_port) ? cs.client_seq : cs.server_seq;
}

uint32_t PcapWriter::peerSeq(uint32_t src_ip, uint16_t src_port,
                              uint32_t dst_ip, uint16_t dst_port) {
    auto& cs = conn_seq_[connKey(src_ip, src_port, dst_ip, dst_port)];
    return isClient(src_ip, src_port, dst_ip, dst_port) ? cs.server_seq : cs.client_seq;
}

void PcapWriter::advanceSeq(uint32_t src_ip, uint16_t src_port,
                             uint32_t dst_ip, uint16_t dst_port, uint32_t bytes) {
    auto& cs = conn_seq_[connKey(src_ip, src_port, dst_ip, dst_port)];
    if (isClient(src_ip, src_port, dst_ip, dst_port)) cs.client_seq += bytes;
    else cs.server_seq += bytes;
}

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
    {
        std::vector<uint8_t> addrs;
        addrs.push_back(uint8_t(src_ip>>24)); addrs.push_back(uint8_t(src_ip>>16));
        addrs.push_back(uint8_t(src_ip>>8));  addrs.push_back(uint8_t(src_ip));
        addrs.push_back(uint8_t(dst_ip>>24)); addrs.push_back(uint8_t(dst_ip>>16));
        addrs.push_back(uint8_t(dst_ip>>8));  addrs.push_back(uint8_t(dst_ip));
        ip.insert(ip.end(), addrs.begin(), addrs.end());
    }
    uint16_t ck = ipChecksum(ip.data(), 20);
    ip[10] = uint8_t(ck >> 8); ip[11] = uint8_t(ck);

    bool has_ack = (tcp_flags != 0x02);

    std::vector<uint8_t> tcp;
    putU16(tcp, src_port);
    putU16(tcp, dst_port);
    {
        uint32_t s = seq_num;
        tcp.push_back(uint8_t(s>>24)); tcp.push_back(uint8_t(s>>16));
        tcp.push_back(uint8_t(s>>8));  tcp.push_back(uint8_t(s));
        uint32_t a = has_ack ? ack_num : 0;
        tcp.push_back(uint8_t(a>>24)); tcp.push_back(uint8_t(a>>16));
        tcp.push_back(uint8_t(a>>8));  tcp.push_back(uint8_t(a));
    }
    tcp.push_back(0x50); // data offset = 5 (20 bytes, no options)
    tcp.push_back(tcp_flags);
    putU16(tcp, 65535);  // window size
    putU16(tcp, 0x0000); // checksum (0 = disabled, Wireshark ignores)
    putU16(tcp, 0x0000); // urgent pointer

    std::vector<uint8_t> frame = buildEthernet();
    frame.insert(frame.end(), ip.begin(),      ip.end());
    frame.insert(frame.end(), tcp.begin(),     tcp.end());
    frame.insert(frame.end(), payload.begin(), payload.end());
    return frame;
}

void PcapWriter::putU16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back(uint8_t(x >> 8)); v.push_back(uint8_t(x));
}
void PcapWriter::putU32le(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back(uint8_t(x));       v.push_back(uint8_t(x >> 8));
    v.push_back(uint8_t(x >> 16)); v.push_back(uint8_t(x >> 24));
}
uint16_t PcapWriter::ipChecksum(const uint8_t* buf, int len) {
    uint32_t sum = 0;
    for (int i = 0; i < len; i += 2)
        sum += (uint32_t(buf[i]) << 8) | buf[i+1];
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return uint16_t(~sum);
}
