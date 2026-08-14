# Go Microservice Layer — Architecture

## 1. Why Go alongside C++?

This project deliberately splits responsibilities between two languages, each doing what it is best at.

### What C++ keeps

The existing C++ simulator handles everything that requires:
- **Protocol encoding precision**: ASN.1 PER/BER (NGAP, S1AP, NAS-EPS), SCTP, GTP-U — these are byte-exact and latency-critical.
- **Nanosecond-level timing**: GTP-U forwarding, NGAP procedure state machines, retransmission timers.
- **Existing codebases**: nrf_sim, udm_sim, amf_sim, smf_sim, upf_sim, gnb_sim — all fully operational.
- **DPDK / kernel bypass** (future): packet-processing at line rate requires C++ or Rust.

### What Go adds

| Concern | Why Go wins here |
|---|---|
| HTTP/JSON SBI layer | `net/http` is production-grade, goroutines handle thousands of concurrent connections trivially |
| Operator control plane | REST API + JWT auth is 50 lines in Go, 500 in C++ |
| Live event streaming | SSE via goroutines + channels maps perfectly to Go's concurrency model |
| Container images | Static binary, 15 MB image, no shared library hell |
| Compile speed | Full rebuild in <3s vs 30-60s for the C++ sim |
| Interview signal | Demonstrates polyglot architecture — a real 5G core production pattern |

In production 5G cores (Open5GS, free5GC, Magma), Go is the dominant language for the SBI (Service-Based Interface) HTTP/2 layer, while C/C++ handles the data plane.

---

## 2. Service Map

```
╔══════════════════════════════════════════════════════════════════════╗
║  [C++ Simulator Layer]                                               ║
║                                                                      ║
║  nrf_sim:29510   udm_sim:29503   smf_sim:29502   upf_sim:8805       ║
║       ▲               ▲               ▲               ▲             ║
║  amf_sim:38412 ────── NRF HTTP /nf-instances ─────────┘             ║
║       ▲                                                              ║
╚═══════╪══════════════════════════════════════════════════════════════╝
        │ HTTP + exec.Command (local) / K8s API (production)
╔═══════╪══════════════════════════════════════════════════════════════╗
║  [Go Microservice Layer]                                             ║
║                                                                      ║
║  auth-service:8081    control-api:8082    event-gateway:8083        ║
║  ┌─────────────┐      ┌─────────────┐    ┌──────────────────┐      ║
║  │ POST /login │      │ GET /metrics│    │ GET /events      │      ║
║  │ GET  /me    │─JWT─▶│ GET /scenarios   │ GET /events/stream│      ║
║  │ GET  /health│      │ POST /start │    │ GET /health      │      ║
║  └─────────────┘      │ POST /stop  │    └──────────────────┘      ║
║                       └─────────────┘           ▲                   ║
║                              │              log watcher             ║
║                              │              nrf poller              ║
║                              └──────────────────┘                   ║
╚══════════════════════════════════════════════════════════════════════╝
        │
  [Client / CI / kubectl]
```

**Request flow example — starting the simulator:**
1. `POST /login` → auth-service returns JWT (role: operator)
2. `POST /scenarios/start` with `Authorization: Bearer <token>` → control-api verifies JWT, launches C++ NFs via exec.Command, returns `{"status":"starting"}`
3. `GET /events/stream` with token → event-gateway SSE stream; log-watcher goroutine tails `/tmp/g5_*.log` and pushes new lines as `data: {...}\n\n`

This diagram predates Kafka/`kpi-consumer` and the C++ NF-to-NF discovery
picture — for the full stack (C++ NRF discovery, Kafka, Kubernetes service
discovery, all connected end to end) see `docs/SYSTEM_ARCHITECTURE.md`.

---

## 3. Go Design Patterns in This Layer

### Goroutines + Channels (event-gateway)

```go
// Two background goroutines share one Publisher interface.
// Neither goroutine knows whether it's writing to memory or Kafka.
go logWatcher(ctx, cfg, store)    // polls log files every 500ms
go nrfPoller(ctx, cfg, store)     // queries NRF every 5s

// SSE handler subscribes to in-memory fan-out channels:
subID, ch := store.Subscribe()
defer store.Unsubscribe(subID)
for ev := range ch {
    fmt.Fprintf(w, "data: %s\n\n", marshal(ev))
    flusher.Flush()
}
```

C++ equivalent would require `std::thread`, `std::condition_variable`, and careful locking. Goroutines are scheduled cooperatively by the Go runtime (G-M-P model), starting at ~2KB stack vs ~8MB for an OS thread.

### Context for Cancellation

```go
ctx, cancel := context.WithCancel(context.Background())
// ... start goroutines that select on ctx.Done() ...
signal.Notify(quit, syscall.SIGTERM)
<-quit
cancel()  // propagates to ALL goroutines that received this ctx
wg.Wait() // wait for clean exit
```

`context.Context` is the Go idiom for deadline/cancellation propagation. Every goroutine that does I/O should accept a `context.Context` parameter and check `ctx.Done()`. This prevents goroutine leaks on shutdown.

### Interface Abstraction (Publisher)

```go
// events/model.go — the ONLY abstraction point for storage backend
type Publisher interface {
    Publish(e SimEvent)
}

// Always: in-memory circular buffer, powers /events and /events/stream
store := events.NewBoundedStore(1000)

// If KAFKA_BROKERS is set: ALSO fan out to Kafka, no other code changes
// (see internal/events/kafka.go — this is implemented, not hypothetical)
pub := &events.FanOutPublisher{Local: store, Kafka: events.NewKafkaPublisher(brokers, topic)}
```

The interface is defined in the `events` package (the domain), not in the storage package. This is the Dependency Inversion Principle — high-level policy (publishing events) does not depend on low-level detail (where they go).

### Graceful Shutdown

All three services follow the same pattern:
```go
quit := make(chan os.Signal, 1)
signal.Notify(quit, os.Interrupt, syscall.SIGTERM)
<-quit                          // block until signal
ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
defer cancel()
srv.Shutdown(ctx)               // drains in-flight HTTP requests
```

K8s sends SIGTERM, then waits `terminationGracePeriodSeconds` (default 30s) before SIGKILL. Our 10s window fits comfortably inside that.

### Middleware Chaining

```go
// RequireRole wraps any http.Handler — composes without framework lock-in.
mux.Handle("/metrics", auth.RequireRole(secret, auth.RoleViewer,
    http.HandlerFunc(myHandler),
))
```

Go's `http.Handler` interface (`ServeHTTP(ResponseWriter, *Request)`) is the composition unit. Chains of middleware are just nested function calls — no magic.

---

## 4. How to Extend

### Kafka — implemented, see `internal/events/kafka.go`

`KafkaPublisher` (via `segmentio/kafka-go`) and `FanOutPublisher` (publishes
to both the local `BoundedStore` and Kafka) are real, not a sketch. Enabled
by setting `KAFKA_BROKERS`; `GET /events` and `/events/stream` deliberately
still read from `BoundedStore`, not Kafka — see `docs/SYSTEM_ARCHITECTURE.md`
Part 4 for why, and `docs/KAFKA_INTERVIEW_PREP.md` for the consumer side
(`cmd/kpi-consumer`) with windowed aggregation, retry/DLQ, and idempotency.

### Adding TLS

```go
srv.ListenAndServeTLS("/etc/tls/tls.crt", "/etc/tls/tls.key")
```

For internal mTLS (service-to-service), configure `tls.Config` with `ClientAuth: tls.RequireAndVerifyClientCert` and load the CA cert. In K8s, use cert-manager to automate rotation.

### Adding Distributed Tracing

Add OpenTelemetry middleware before routing:
```go
import "go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
handler = otelhttp.NewHandler(mux, "auth-service")
```
Traces appear in Jaeger/Tempo. The trace ID propagates via `traceparent` header through all three Go services and can be correlated with logs (inject trace ID into slog fields).

### Adding Prometheus Histograms

Replace manual counters in control-api with:
```go
import "github.com/prometheus/client_golang/prometheus"
requestDuration = prometheus.NewHistogramVec(prometheus.HistogramOpts{...}, []string{"method","path","status"})
```
Register with `prometheus.MustRegister` and serve via `promhttp.Handler()`.

---

## 5. What Stays in C++ Forever

| Component | Reason |
|---|---|
| NGAP / S1AP ASN.1 PER encoder/decoder | Byte-exact, 3GPP-specified, existing tested implementation |
| SCTP transport (amf_sim ↔ gnb_sim) | OS-level socket API; Go's `net` package lacks SCTP support |
| GTP-U encapsulation (upf_sim) | Requires raw socket access and kernel bypass for performance |
| UE procedure state machines | Timing-sensitive (T3550, T3560 timers per 3GPP TS 24.501) |
| NAS-EPS / 5G-NAS encoding | Complex ASN.1 structures; C++ struct-based approach is correct here |

The architectural split is: **C++ owns the radio/transport/protocol plane; Go owns the management/control/observability plane.** This mirrors how Nokia, Ericsson, and Rakuten Mobile structure their 5G products.
