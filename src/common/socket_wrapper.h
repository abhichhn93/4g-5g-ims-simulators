#pragma once
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <string>

// ============================================================
// TCP SOCKET WRAPPER — RAII, framed messages
//
// Used by: eNB↔MME (S1AP, port 38412), MME↔HSS (Diameter, port 3868)
//
// REAL S1AP: SCTP (RFC 4960), port 36412
//   - Message boundaries (no length prefix needed)
//   - Multi-streaming (one SCTP stream per UE — no head-of-line blocking)
//   - Multi-homing (failover to backup IP)
// OUR SIM: TCP with 4-byte length prefix for message framing
// ============================================================
class Socket {
public:
    Socket() : fd_(-1) {}
    explicit Socket(int fd) : fd_(fd) {}
    Socket(Socket&& o) noexcept : fd_(o.fd_) { o.fd_=-1; }
    Socket& operator=(Socket&& o) noexcept { if(this!=&o){close();fd_=o.fd_;o.fd_=-1;} return*this; }
    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    ~Socket() { close(); }

    void close() { if(fd_>=0){::close(fd_);fd_=-1;} }
    int  fd()    const { return fd_; }
    bool valid() const { return fd_>=0; }

    static Socket createServer(const char* ip, uint16_t port, int backlog=5) {
        int fd = ::socket(AF_INET,SOCK_STREAM,0);
        if(fd<0) throw std::runtime_error("socket() failed");
        int opt=1; ::setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port);
        ::inet_pton(AF_INET,ip,&a.sin_addr);
        if(::bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0){::close(fd);throw std::runtime_error("bind() failed port "+std::to_string(port));}
        if(::listen(fd,backlog)<0){::close(fd);throw std::runtime_error("listen() failed");}
        return Socket(fd);
    }
    Socket accept() const {
        sockaddr_in c{}; socklen_t l=sizeof(c);
        int cfd=::accept(fd_,reinterpret_cast<sockaddr*>(&c),&l);
        if(cfd<0) throw std::runtime_error("accept() failed");
        return Socket(cfd);
    }
    static Socket connectTo(const char* ip, uint16_t port) {
        int fd=::socket(AF_INET,SOCK_STREAM,0);
        if(fd<0) throw std::runtime_error("socket() failed");
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port);
        ::inet_pton(AF_INET,ip,&a.sin_addr);
        if(::connect(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0){::close(fd);throw std::runtime_error("connect() failed to "+std::string(ip)+":"+std::to_string(port));}
        return Socket(fd);
    }

    bool sendAll(const void* buf, uint32_t len) const {
        const char* p=static_cast<const char*>(buf); uint32_t r=len;
        while(r>0){ssize_t n=::send(fd_,p,r,0);if(n<=0)return false;p+=n;r-=uint32_t(n);}
        return true;
    }
    bool recvAll(void* buf, uint32_t len) const {
        char* p=static_cast<char*>(buf); uint32_t r=len;
        while(r>0){ssize_t n=::recv(fd_,p,r,0);if(n<=0)return false;p+=n;r-=uint32_t(n);}
        return true;
    }

    // Wire: [4B payload_len][payload]
    bool sendFrame(const std::vector<uint8_t>& frame) const { return sendAll(frame.data(),uint32_t(frame.size())); }
    bool recvFrame(std::vector<uint8_t>& payload) const {
        uint32_t plen=0; if(!recvAll(&plen,4)) return false;
        if(plen==0||plen>65536) return false;
        payload.resize(plen); return recvAll(payload.data(),plen);
    }

    bool hasData(int ms) const {
        if(!valid()) return false;
        fd_set r; FD_ZERO(&r); FD_SET(fd_,&r);
        timeval tv{ms/1000,(ms%1000)*1000};
        return ::select(fd_+1,&r,nullptr,nullptr,&tv)>0;
    }
    bool hasConnection(int ms) const { return hasData(ms); }

private:
    int fd_;
};

// ============================================================
// UDP SOCKET WRAPPER — RAII, connectionless
//
// Used by: MME↔S-GW (GTP-C S11, port 2123), S-GW↔P-GW (GTP-C S5, port 2124)
//
// REAL GTP-C: UDP (RFC 768) with GTP header (TEID routing)
//   - Connectionless: no TCP handshake
//   - Each datagram is a complete PDU (no length prefix needed!)
//   - TEID in GTP header routes the message to the right UE context
// OUR SIM: UDP with our TLV frame format (no length prefix)
//
// INTERVIEW Q: "Why UDP for GTP-C, not TCP?"
// ANSWER: "GTP-C is request-response with built-in retransmission
//   (sequence numbers + T3-RESPONSE timers in TS 29.274 §4.2).
//   Using TCP would add unnecessary connection state on the core network.
//   Also, GTP-U (user plane) uses UDP for low latency — using the same
//   transport for control plane simplifies the stack."
// ============================================================
class UdpSocket {
public:
    UdpSocket() : fd_(-1) {}
    UdpSocket(UdpSocket&& o) noexcept : fd_(o.fd_) { o.fd_=-1; }
    UdpSocket& operator=(UdpSocket&& o) noexcept { if(this!=&o){close();fd_=o.fd_;o.fd_=-1;} return*this; }
    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;
    ~UdpSocket() { close(); }

    void close() { if(fd_>=0){::close(fd_);fd_=-1;} }
    bool valid() const { return fd_>=0; }

    // Create and bind to a local port (server or fixed-port client)
    static UdpSocket bind(const char* ip, uint16_t port) {
        int fd=::socket(AF_INET,SOCK_DGRAM,0);
        if(fd<0) throw std::runtime_error("UDP socket() failed");
        int opt=1; ::setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port);
        ::inet_pton(AF_INET,ip,&a.sin_addr);
        if(::bind(fd,reinterpret_cast<sockaddr*>(&a),sizeof(a))<0){::close(fd);throw std::runtime_error("UDP bind() failed port "+std::to_string(port));}
        UdpSocket s; s.fd_=fd; return s;
    }

    // Send UDP datagram (from MessageWriter::udpPayload() — no length prefix)
    bool sendTo(const std::vector<uint8_t>& payload, const char* ip, uint16_t port) const {
        sockaddr_in a{}; a.sin_family=AF_INET; a.sin_port=htons(port);
        ::inet_pton(AF_INET,ip,&a.sin_addr);
        ssize_t n=::sendto(fd_,payload.data(),payload.size(),0,reinterpret_cast<sockaddr*>(&a),sizeof(a));
        return n==ssize_t(payload.size());
    }

    // Also send using pre-filled sockaddr (used when replying to recvfrom sender)
    bool sendToAddr(const std::vector<uint8_t>& payload, const sockaddr_in& addr) const {
        ssize_t n=::sendto(fd_,payload.data(),payload.size(),0,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr));
        return n==ssize_t(payload.size());
    }

    // Receive one UDP datagram — fills payload AND records sender address
    bool recvFrom(std::vector<uint8_t>& payload, sockaddr_in& sender, int max_bytes=8192) {
        payload.resize(max_bytes);
        socklen_t l=sizeof(sender);
        ssize_t n=::recvfrom(fd_,payload.data(),payload.size(),0,reinterpret_cast<sockaddr*>(&sender),&l);
        if(n<=0) return false;
        payload.resize(n);
        return true;
    }

    bool hasData(int ms) const {
        if(!valid()) return false;
        fd_set r; FD_ZERO(&r); FD_SET(fd_,&r);
        timeval tv{ms/1000,(ms%1000)*1000};
        return ::select(fd_+1,&r,nullptr,nullptr,&tv)>0;
    }

    // Blocking receive with timeout — returns false on timeout or error
    bool recvWithTimeout(std::vector<uint8_t>& payload, sockaddr_in& sender, int timeout_ms) {
        if(!hasData(timeout_ms)) return false;
        return recvFrom(payload,sender);
    }

private:
    int fd_;
};
