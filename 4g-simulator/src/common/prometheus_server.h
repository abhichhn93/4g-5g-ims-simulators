#pragma once
// ============================================================
// PrometheusServer — raw-TCP HTTP/1.1 server on port 9090
//
// Exposes GET /metrics in Prometheus text exposition format.
// No external libraries — raw POSIX sockets only.
//
// CLOUD-NATIVE CONTEXT:
//   In production: each MME pod runs this on :9090/metrics.
//   Prometheus scrapes every 15s (configurable via scrape_interval).
//   ServiceMonitor (K8s CRD) targets pods with label app=mme-sim.
//   Grafana dashboard reads from Prometheus and plots P95/P99 timeseries.
//
// INTERVIEW Q: "How does Prometheus scraping work?"
// ANSWER: "Pull model. Prometheus server sends HTTP GET /metrics to each
//   target at scrape_interval. Target returns text: '# TYPE name gauge\n
//   name{label} value\n'. No push, no agent — simpler ops model."
//
// KUBERNETES HPA:
//   custom.metrics.k8s.io/mme_attach_p99_ms → HPA scale trigger
//   When P99 > 1500ms → scale out MME replicas
//   When P99 < 500ms  → scale in (minReplicas=1)
// ============================================================
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <netinet/in.h>
#include <unistd.h>
#include <sys/socket.h>
#include "common/metrics.h"
#include "common/logger.h"

class PrometheusServer {
public:
    explicit PrometheusServer(std::shared_ptr<Metrics> metrics, uint16_t port = 9090)
        : metrics_(std::move(metrics)), port_(port), stop_(false), fd_(-1)
    {}

    ~PrometheusServer() { stop(); }

    void start() {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) { Logger::warn("PROM","socket() failed"); return; }

        int opt = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port        = htons(port_);

        if (::bind(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
            Logger::warn("PROM","bind() on port " + std::to_string(port_) + " failed");
            ::close(fd_); fd_ = -1; return;
        }
        ::listen(fd_, 8);
        Logger::sys("Prometheus /metrics endpoint: http://localhost:" + std::to_string(port_) + "/metrics");
        Logger::sys("  Scrape with: curl http://localhost:" + std::to_string(port_) + "/metrics");

        thread_ = std::thread([this]{ serve(); });
    }

    void stop() {
        stop_.store(true);
        if (fd_ >= 0) { ::shutdown(fd_, SHUT_RDWR); ::close(fd_); fd_ = -1; }
        if (thread_.joinable()) thread_.join();
    }

private:
    std::shared_ptr<Metrics> metrics_;
    uint16_t port_;
    std::atomic<bool> stop_;
    int fd_;
    std::thread thread_;

    void serve() {
        Logger::sys("[PROM] HTTP scrape thread started on :" + std::to_string(port_));
        while (!stop_.load()) {
            sockaddr_in client{};
            socklen_t clen = sizeof(client);

            // accept() blocks; fd_ close() from stop() will unblock it
            int cfd = ::accept(fd_, (sockaddr*)&client, &clen);
            if (cfd < 0) break;

            handleRequest(cfd);
            ::close(cfd);
        }
        Logger::sys("[PROM] HTTP scrape thread stopped");
    }

    void handleRequest(int cfd) {
        char buf[512] = {};
        ::recv(cfd, buf, sizeof(buf) - 1, 0);

        std::string_view req(buf);
        bool is_metrics = req.find("GET /metrics") != std::string_view::npos;
        bool is_health  = req.find("GET /health")  != std::string_view::npos
                       || req.find("GET /")         != std::string_view::npos;

        std::string body;
        std::string status;

        if (is_metrics) {
            body   = metrics_ ? metrics_->prometheusText() : "# no metrics\n";
            status = "200 OK";
        } else if (is_health) {
            body   = R"({"status":"ok","node":"mme-sim","port":)" +
                     std::to_string(port_) + "}\n";
            status = "200 OK";
        } else {
            body   = "404 Not Found\nTry GET /metrics or GET /health\n";
            status = "404 Not Found";
        }

        // Prometheus text format content-type: text/plain; version=0.0.4
        std::string content_type = is_metrics
            ? "text/plain; version=0.0.4; charset=utf-8"
            : "application/json";

        std::string resp =
            "HTTP/1.1 " + status + "\r\n"
            "Content-Type: " + content_type + "\r\n"
            "Content-Length: " + std::to_string(body.size()) + "\r\n"
            "Connection: close\r\n"
            "\r\n" + body;

        ::send(cfd, resp.data(), resp.size(), 0);
    }
};
