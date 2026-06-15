#pragma once
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <cstdint>
#include <cstring>
#include <vector>
#include <stdexcept>
#include <string>

// ============================================================
// TCP SOCKET WRAPPER — RAII, framed messages
//
// Copied from the 4G simulator's common/socket_wrapper.h (same
// building block, same RAII behaviour). Used here for:
//   - N2  (gNB <-> AMF): length-prefixed JSON frames, see wire.h
//   - SBI (AMF <-> UDM): real HTTP/1.1 over TCP, see wire.h
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
    // Resolves `host` via getaddrinfo, so it accepts BOTH a literal IP
    // (e.g. "127.0.0.1") AND a hostname (Docker Compose service name /
    // K8s Service DNS name, e.g. "amf-sim", "g5-amf-svc"). The previous
    // version used inet_pton(), which silently fails (-> 0.0.0.0) on a
    // hostname -- that bug only surfaced once these binaries ran in
    // separate containers and started addressing each other by name.
    static Socket connectTo(const char* host, uint16_t port) {
        addrinfo hints{}; hints.ai_family=AF_INET; hints.ai_socktype=SOCK_STREAM;
        addrinfo* res=nullptr;
        if(::getaddrinfo(host,std::to_string(port).c_str(),&hints,&res)!=0 || !res)
            throw std::runtime_error("getaddrinfo() failed for "+std::string(host));
        int fd=::socket(res->ai_family,res->ai_socktype,res->ai_protocol);
        if(fd<0){::freeaddrinfo(res);throw std::runtime_error("socket() failed");}
        if(::connect(fd,res->ai_addr,res->ai_addrlen)<0){
            ::freeaddrinfo(res);::close(fd);
            throw std::runtime_error("connect() failed to "+std::string(host)+":"+std::to_string(port));
        }
        ::freeaddrinfo(res);
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
