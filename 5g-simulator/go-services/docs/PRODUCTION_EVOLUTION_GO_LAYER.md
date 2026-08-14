# Production Evolution of the Go Layer

What we built is production-shaped but not production-hardened. This document maps each component from its current "demo" state to what a real 5G operator deployment would require. Use this as a roadmap and as interview material ("I know the gaps and the upgrade path").

---

## 1. Authentication: Hardcoded Users → IdP Integration

**Current**: Two users (`admin`, `viewer`) with bcrypt hashes built at startup. No user management, no refresh tokens, no session revocation.

**Production**:
- **LDAP/OAuth2**: replace the `buildUserDB()` map with a call to an Identity Provider (Keycloak, Okta, Azure AD). `POST /login` becomes an OAuth2 authorization code flow; our service validates the IdP-issued JWT rather than issuing its own.
- **Short-lived tokens**: reduce `JWT_EXPIRY_HOURS` from 24 to 1. Add a `POST /refresh` endpoint that accepts a long-lived refresh token (stored in an `HttpOnly` cookie) and returns a new short-lived access token.
- **mTLS for service-to-service**: control-api → NRF and event-gateway → NRF should use client certificates (mutual TLS), not JWTs. JWTs are for human/operator authentication; mTLS is for machine-to-machine.
- **Token revocation**: add a Redis `SET jwt-jti:<jti> 1 EX <expiry>` on logout. Middleware checks Redis before accepting. Without this, a stolen JWT is valid until expiry.

```go
// Upgrade path: one function change in auth-service/main.go
// Today:
u, ok := users[body.Username]
bcrypt.CompareHashAndPassword(u.hash, []byte(body.Password))

// Production:
claims, err := ldap.Authenticate(body.Username, body.Password)  // LDAP call
// or:
token, err := oauth2Config.Exchange(ctx, code)  // OAuth2 code exchange
```

---

## 2. Config Management: Env Vars → Vault

**Current**: `JWT_SECRET` comes from a K8s Secret (base64-encoded, stored in etcd). This is acceptable for dev; etcd at-rest encryption is required for production.

**Production**:
- **HashiCorp Vault**: mount secrets as a sidecar (Vault Agent Injector) or use the Vault API at startup. Vault provides: secret versioning, audit logs, dynamic secrets (short-lived credentials), automatic rotation.
- **Sealed Secrets** (Bitnami): encrypt K8s Secrets with a cluster-specific key. The sealed secret can be committed to git; only the cluster can decrypt it. Simpler than Vault for small teams.
- **Never log secrets**: audit all `slog.Info` calls to ensure no config struct is printed. Add a `String() string` method to `Config` that redacts `JWTSecret`.

```go
// Redact sensitive fields from accidental logging
func (c Config) String() string {
    return fmt.Sprintf("Config{AuthPort:%d, NRFAddr:%s, JWTSecret:[REDACTED]}",
        c.AuthPort, c.NRFAddr)
}
```

---

## 3. Events: In-Memory BoundedStore → Kafka — IMPLEMENTED

**Status update**: this section used to describe a hypothetical upgrade. It's now real. `internal/events/kafka.go` implements `KafkaPublisher` (using `segmentio/kafka-go`, not the `sarama` sketch this doc originally showed — kafka-go's producer/consumer-group API needs less boilerplate, which mattered more than matching the sketch once it came time to actually write it). `cmd/kpi-consumer/main.go` is the consumer-group side: group ID `kpi-readers`, topic `5g-sim-events`, windowed KPI aggregation, retry+DLQ, idempotent counting. Local dev: `docker/docker-compose.kafka.yml` (single KRaft broker, no ZooKeeper). See `docs/KAFKA_INTERVIEW_PREP.md` for the full walkthrough.

**What changed in `event-gateway`**: `BoundedStore` was NOT replaced — it's still what powers `/events` and `/events/stream` (SSE), unaffected by Kafka being up, down, or misconfigured. Instead, `main.go` now conditionally wraps it in a `FanOutPublisher` (also in `internal/events/kafka.go`) that publishes to both `BoundedStore` (fast, local, for the live UI) and Kafka (durable, replayable, for downstream consumers like `kpi-consumer`) when `KAFKA_BROKERS` is set. This is a deliberate difference from the original "replace BoundedStore" sketch below: SSE latency for a live-view tool shouldn't depend on Kafka being healthy.

**Partition key**: SUPI (falls back to Node for NF-level events with no SUPI) — same reasoning the original sketch had: Kafka only orders within a partition, so pinning one subscriber's events to one partition preserves per-UE ordering without needing global ordering across the whole topic.

**What's still a gap vs. a real operator deployment** (interview material — "I know what's next"):
- Single broker, replication factor 1 (`docker-compose.kafka.yml`) — production needs 3+ brokers so a broker loss doesn't lose data.
- No auth/TLS on the Kafka listener (PLAINTEXT) — production needs SASL/mTLS between services and brokers.
- No schema registry / Avro — events are raw JSON; a real telecom-scale pipeline would use Avro or Protobuf with a schema registry to catch producer/consumer schema drift at write time instead of at a downstream consumer's `json.Unmarshal`.
- `kpi-consumer`'s window state is in-process memory, not a durable state store (Kafka Streams / Flink would checkpoint window state so a pod restart doesn't lose an in-flight window's partial counts — this demo accepts that a restart mid-window undercounts that one window, which is the tradeoff called out in `closeExpiredWindows`'s doc comment).

**What Kafka gives you here, concretely** (not hypothetically): at-least-once delivery (`kpi-consumer` compensates with idempotent counting), replay-ability (any consumer group can re-read from any offset), consumer-group load-splitting across `kpi-consumer` replicas without any coordination code of our own, and a durable buffer between event-gateway (fast producer) and downstream aggregation (which can fall behind and catch up independently).

---

## 4. Metrics: Manual Text → prometheus/client_golang

**Partially implemented**: `cmd/kpi-consumer` uses the real `prometheus/client_golang` library (`promauto` + `promhttp.Handler()`) for `kpi_consumer_messages_processed_total`, `kpi_consumer_dlq_total`, `kpi_consumer_lag`. `control-api`'s `/metrics` below is still the manual/older approach — the two coexisting in one repo is itself a fair interview talking point ("here's the before and after of adopting the real client library").

**Current** (control-api specifically): writes Prometheus text format manually. No histograms, no labels on status codes, no scrape registration.

**Production**:

```go
import "github.com/prometheus/client_golang/prometheus"
import "github.com/prometheus/client_golang/prometheus/promhttp"

var (
    httpRequestDuration = prometheus.NewHistogramVec(
        prometheus.HistogramOpts{
            Name:    "http_request_duration_seconds",
            Buckets: []float64{.005, .01, .025, .05, .1, .25, .5, 1, 2.5},
        },
        []string{"service", "method", "path", "status"},
    )
    scenarioStatus = prometheus.NewGauge(prometheus.GaugeOpts{
        Name: "go_scenario_status",
        Help: "1 if scenario is running, 0 otherwise",
    })
)

func init() {
    prometheus.MustRegister(httpRequestDuration, scenarioStatus)
}

// Replace manual /metrics handler with:
mux.Handle("/metrics", promhttp.Handler())
```

Add a `recording middleware` that observes every request into `httpRequestDuration`. Alerts: p99 latency > 500ms, error rate > 1%, scenario_status == 0 for > 5min.

---

## 5. Distributed Tracing: None → OpenTelemetry

**Current**: no trace context; log lines from different services cannot be correlated for a single scenario-start flow.

**Production**:

```go
import "go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
import "go.opentelemetry.io/otel"

// Wrap the entire mux — auto-creates a span per request
handler := otelhttp.NewHandler(mux, "auth-service",
    otelhttp.WithTracerProvider(otel.GetTracerProvider()),
)

// In log middleware: inject trace ID into every log line
traceID := trace.SpanFromContext(r.Context()).SpanContext().TraceID().String()
slog.InfoContext(r.Context(), "request", "trace_id", traceID, ...)
```

Export spans to Jaeger (dev) or Tempo (production). The `traceparent` W3C header propagates trace context from curl → auth-service → control-api → NRF HTTP call. Full distributed trace is visible in Grafana.

---

## 6. Retries and Circuit Breaking

**Current**: NRF calls have a 3s timeout and no retry. A transient NRF restart returns `nrf_reachable: false` for the entire poll cycle.

**Production**:

```go
import "github.com/sony/gobreaker"

// Circuit breaker: after 5 consecutive failures, open for 30s
cb := gobreaker.NewCircuitBreaker(gobreaker.Settings{
    Name:        "nrf-client",
    MaxRequests: 3,               // half-open: allow 3 probes
    Interval:    10 * time.Second,
    Timeout:     30 * time.Second,
    ReadyToTrip: func(counts gobreaker.Counts) bool {
        return counts.ConsecutiveFailures > 5
    },
})

// Wrap NRF calls through the circuit breaker
result, err := cb.Execute(func() (any, error) {
    return fetchNFIDs(cfg.NRFAddr)
})
```

**State machine**: Closed (normal) → Open (failing fast) → Half-Open (probing) → Closed. This prevents a cascade where a slow NRF makes all control-api handlers block for 3s each.

---

## 7. Control Bridge: exec.Command → K8s client-go

**Current**: control-api launches C++ binaries with `os/exec`. This requires the binaries to be in the same container or on a shared filesystem. Fine for local/Docker-compose; wrong for K8s.

**Production**:

```go
import "k8s.io/client-go/kubernetes"
import "k8s.io/client-go/rest"

// In-cluster config (reads ServiceAccount token automatically)
cfg, _ := rest.InClusterConfig()
clientset, _ := kubernetes.NewForConfig(cfg)

// Scale NRF deployment to 1 replica = "start"
patch := []byte(`{"spec":{"replicas":1}}`)
clientset.AppsV1().Deployments("5g-sim").Patch(
    ctx, "g5-nrf", types.MergePatchType, patch, metav1.PatchOptions{},
)

// Watch pod events to know when NRF is actually ready
watch, _ := clientset.CoreV1().Pods("5g-sim").Watch(ctx, metav1.ListOptions{
    LabelSelector: "app=g5-nrf",
})
for event := range watch.ResultChan() {
    if event.Type == watchv1.EventType("MODIFIED") { /* check readiness */ }
}
```

The control-api ServiceAccount needs RBAC: `get`, `patch`, `list`, `watch` on `deployments` and `pods` in the `5g-sim` namespace.

---

## 8. SSE → WebSocket or Kafka Consumer

**Current**: SSE (Server-Sent Events) is one-directional (server → client), text-only, and reconnects automatically. Good for log tailing dashboards. No delivery guarantee — if the client disconnects for 5s, it misses events.

**Production options**:
- **WebSocket**: bidirectional, binary or text, lower latency. Use `gorilla/websocket`. Client can send filter commands (subscribe to AMF events only). More complex: requires WebSocket upgrade handshake.
- **Kafka consumer in the browser**: not feasible directly. Instead, a websocket proxy goroutine reads from a Kafka consumer group and writes to the WS connection. Client reconnect = consumer group re-join = resume from last committed offset = **no missed events**.
- **At-least-once guarantee**: Kafka offset commit after the browser ACKs. Without this, a browser crash loses the in-flight event.

---

## 9. Race Detection in CI

**Current**: code was written with correct mutex usage (verified by inspection). No automated race detection runs.

**Production**:

```yaml
# .github/workflows/go.yml
- name: Run tests with race detector
  run: go test -race -count=1 ./...

# For services without tests yet: run the binary briefly under -race
- name: Race-check startup
  run: |
    go run -race ./cmd/auth-service/ &
    sleep 2
    curl -f http://localhost:8081/health
    kill %1
```

The race detector adds ~10% CPU overhead — acceptable in CI, too slow for production. Enable it in staging environments with load testing (Locust/k6) to catch races under concurrent load.

---

## 10. Profiling with pprof

**Current**: no profiling endpoint. If the event-gateway goroutine count grows unexpectedly, there's no way to inspect it.

**Production**:

```go
import _ "net/http/pprof"  // side-effect import registers /debug/pprof handlers

// Serve pprof on a separate internal port (never expose on the public port)
go http.ListenAndServe("localhost:6060", nil)
```

Then:
```bash
# Goroutine leak check: see all live goroutines
go tool pprof http://localhost:6060/debug/pprof/goroutine

# CPU profile: 30s sample
go tool pprof http://localhost:6060/debug/pprof/profile?seconds=30

# Memory profile
go tool pprof http://localhost:6060/debug/pprof/heap
```

In K8s: forward the pprof port with `kubectl port-forward` — never expose it via a LoadBalancer Service.
