# OpenTelemetry + gRPC — Interview Guide

**Interactive visual version:** https://claude.ai/code/artifact/0127748a-1dc3-4c69-bbfc-980251a0aa5c
(animated pipeline diagram, clickable Q&A, span waterfall — open in browser for interview prep)

---

## What Was Built

All 4 Go microservices are now instrumented. One new binary was added.

| File | What it does |
|---|---|
| `internal/telemetry/telemetry.go` | Single `Init()` sets up TracerProvider + MeterProvider globally |
| `proto/auth.proto` | Protobuf service contract for gRPC CheckToken RPC |
| `internal/grpc/authpb/` | protoc-generated Go code (do not edit manually) |
| `internal/grpc/server.go` | AuthServer + interceptors + metadataCarrier |
| `cmd/grpc-auth/main.go` | Standalone gRPC binary on :50051 |
| `cmd/auth-service/main.go` | + otelhttp + bcrypt.compare + jwt.issue child spans |
| `cmd/event-gateway/main.go` | + Int64Counter (events by node) + UpDownCounter (subscribers) |
| `cmd/control-api/main.go` | + scenario.start span + nf.launch child spans per binary |
| `cmd/kpi-consumer/main.go` | + Float64Gauge + Int64Counter alongside existing promauto |
| `docker/docker-compose-otel.yml` | OTEL Collector + Jaeger + Prometheus |
| `docker/otel-collector-config.yaml` | Collector pipeline: OTLP receiver → batch → Jaeger + Prometheus |

---

## 1. The Big Picture — Two Pipelines in One `Init()` Call

```
TRACES:
  service → TracerProvider → BatchSpanProcessor → OTLP gRPC → Collector :4317 → Jaeger :16686

METRICS:
  service → MeterProvider → Prometheus bridge → DefaultRegisterer → /metrics endpoint
```

Both pipelines are wired in `telemetry.Init()` at `internal/telemetry/telemetry.go`.

Every service calls `telemetry.Init(ctx, "service-name", cfg.OTLPEndpoint)` once in `main()`.  
If `OTLP_ENDPOINT` is empty, Init is a no-op — services still run without a Collector.

---

## 2. Traces — How Spans Work

### What a span is

A span = one unit of work with a start time, end time, and status.

```
Span fields:
  trace_id   "4bf92f3577b34da6a3ce929d0e0e4736"  ← 128-bit, SAME across all services
  span_id    "00f067aa0ba902b7"                   ← 64-bit, unique per span
  parent_id  "a2fb4a1d..."                        ← links to parent span
  name       "bcrypt.compare"
  service    "auth-service"                       ← from resource.ServiceName
  status     OK | Error
  duration   104ms
  attributes { "http.method": "POST", "http.status_code": 200 }
```

### What you see in Jaeger for POST /login

```
HTTP POST /login              [auth-service · otelhttp · automatic]   106ms ────────────────
  └─ bcrypt.compare           [auth-service · manual]                 104ms ──────────────
  └─ jwt.issue                [auth-service · manual]                   1ms █
```

The indent = parent_id linkage. All three share the same `trace_id`.

### How to create spans — code map

**Automatic (HTTP layer)** — `cmd/auth-service/main.go`
```go
// otelhttp.NewHandler wraps the whole mux. Every request gets a span named
// "auth-service" with HTTP method + status as attributes. No per-handler code needed.
handler := otelhttp.NewHandler(logMiddleware(mux), "auth-service",
    otelhttp.WithTracerProvider(otel.GetTracerProvider()),
)
```

**Manual child span** — `cmd/auth-service/main.go` inside `/login` handler
```go
// r.Context() carries the HTTP span otelhttp created.
// tracer.Start() makes bcrypt.compare a CHILD of that HTTP span.
_, bcryptSpan := tracer.Start(r.Context(), "bcrypt.compare")
err := bcrypt.CompareHashAndPassword(u.hash, []byte(body.Password))
if err != nil {
    bcryptSpan.RecordError(err)                    // attaches error detail to span
    bcryptSpan.SetStatus(otelcodes.Error, "...")
    bcryptSpan.End()
    return
}
bcryptSpan.SetStatus(otelcodes.Ok, "")
bcryptSpan.End() // MUST call End() — this records the duration
```

**Span with attributes** — `cmd/control-api/main.go` in `ScenarioState.start()`
```go
ctx, span := s.tracer.Start(ctx, "scenario.start",
    trace.WithAttributes(attribute.String("sim_bin_dir", cfg.SimBinDir)),
)
defer span.End()

// per-NF child span inside the loop:
_, nfSpan := s.tracer.Start(ctx, "nf.launch",
    trace.WithAttributes(attribute.String("nf", bin)),
)
defer nfSpan.End()
```

**Span event** — `cmd/control-api/main.go` in `ScenarioState.stop()`
```go
// span.AddEvent() = timestamped annotation on the span (shows in Jaeger detail view)
span.AddEvent("sigkill_sent")
```

### TracerProvider setup — `internal/telemetry/telemetry.go`

```go
tp := sdktrace.NewTracerProvider(
    // BatchSpanProcessor: buffers spans, sends in bulk every 5s or 512 spans.
    // Without batching: one HTTP call per span = catastrophic at high throughput.
    sdktrace.WithBatcher(traceExporter),
    sdktrace.WithResource(res),          // stamps service.name on every span
    sdktrace.WithSampler(sdktrace.AlwaysSample()), // 100% sampling, fine for dev
    // Production: sdktrace.TraceIDRatioBased(0.01) for 1% sampling
)
otel.SetTracerProvider(tp)
```

### W3C Trace Context propagation (traceparent header)

```
traceparent: 00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01
              │  │                                │                 └─ flags (01=sampled)
              │  └─ trace-id (128-bit, THE SAME across all services)
              └─ version (always 00)
                                                  └─ parent-id (64-bit, the calling span)
```

How it flows:
1. otelhttp injects `traceparent` into outgoing HTTP response headers
2. The next service's otelhttp extracts it from incoming request headers
3. New span's `parent_id` = extracted span id
4. Same `trace_id` → Jaeger shows one waterfall across services

Configured in `telemetry.Init()`:
```go
otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
    propagation.TraceContext{},  // W3C traceparent / tracestate
    propagation.Baggage{},       // W3C Baggage (optional k=v along the trace)
))
```

---

## 3. Metrics — The Prometheus Bridge

### How OTEL metrics and promauto coexist

The Prometheus exporter (`otelprom.New()`) registers itself as a `prometheus.Collector`
in `prometheus.DefaultRegisterer`. Both systems write to the same registry.

In `/metrics` output from kpi-consumer you see both:
```
# promauto counter (existing)
kpi_consumer_messages_processed_total 42
kpi_consumer_dlq_total 0
kpi_consumer_lag 3

# OTEL metrics via bridge (new)
kpi_consumer_window_closed_total{node="AMF"} 5
kpi_consumer_window_closed_total{node="SMF"} 3
kpi_consumer_window_health_pct{node="AMF"} 97.5
```

Set up in `internal/telemetry/telemetry.go`:
```go
promExporter, _ := otelprom.New()  // registers in DefaultRegisterer automatically
mp := sdkmetric.NewMeterProvider(
    sdkmetric.WithReader(promExporter),  // bridge: OTEL instruments → Prometheus
    sdkmetric.WithResource(res),
)
otel.SetMeterProvider(mp)
```

### The three instrument types

| OTEL Type | Goes | Maps to Prometheus | Use for |
|---|---|---|---|
| `Int64Counter` | only UP | Counter (`_total` suffix) | requests, events, errors |
| `Int64UpDownCounter` | UP and DOWN | Gauge | active connections, queue depth |
| `Float64Gauge` | any value | Gauge | percentages, rates, temperatures |

**Int64Counter** — `cmd/event-gateway/main.go`
```go
eventsPublished, _ := meter.Int64Counter(
    "event_gateway.events_published",
    metric.WithDescription("Total SimEvents published, by source node"),
    metric.WithUnit("{event}"),
)
// in the publish path:
eventsPublished.Add(ctx, 1, metric.WithAttributes(attribute.String("node", string(node))))
// → Prometheus: event_gateway_events_published_total{node="AMF"} 42
```

**Int64UpDownCounter** — `cmd/event-gateway/main.go` in `sseStream`
```go
subscriberCount, _ := meter.Int64UpDownCounter("event_gateway.subscribers_active")
// on SSE connect:   subscriberCount.Add(ctx, +1)
// on SSE disconnect: subscriberCount.Add(ctx, -1)
// Can go negative if you have a bug — use atomic.Int64 to cross-check
```

**Float64Gauge** — `cmd/kpi-consumer/main.go`
```go
otelWindowHealthPct, _ = meter.Float64Gauge(
    "kpi.consumer.window.health_pct",
    metric.WithUnit("%"),
)
// after a window closes:
otelWindowHealthPct.Record(ctx, 97.5, metric.WithAttributes(attribute.String("node", "AMF")))
```

### High cardinality warning

Labels must have LOW cardinality (few possible values). Node type (6 values) = fine.
If you labelled by user_id or trace_id (millions of values), Prometheus creates millions
of time series and OOMs. This is the most common Prometheus anti-pattern in interviews.

---

## 4. gRPC — The AuthService

### Why a gRPC service

Other microservices need to validate JWT tokens without duplicating `auth.Verify()`.
The gRPC AuthService centralises this. It is the "token introspection" pattern.

### How a gRPC call works (vs HTTP)

| | gRPC | HTTP/1.1 REST |
|---|---|---|
| Transport | HTTP/2 (multiplexed, header compressed) | HTTP/1.1 (one req/conn) |
| Serialization | Protobuf (binary, 3-10x smaller) | JSON (text) |
| Contract | `.proto` file — compiler-enforced | OpenAPI (optional, not enforced) |
| Code gen | `protoc` generates client stubs + server interface | optional |
| Discovery | server reflection (`grpcurl -plaintext localhost:50051 list`) | Swagger UI |

### The proto definition — `proto/auth.proto`

```proto
service AuthService {
  rpc CheckToken (AuthCheckRequest) returns (AuthCheckResponse);
}

message AuthCheckRequest {
  string token = 1;   // field number 1 — used in protobuf wire encoding
}

message AuthCheckResponse {
  bool   valid    = 1;
  string username = 2;
  string role     = 3;  // "viewer" | "operator" | "admin"
  string reason   = 4;  // non-empty when valid=false
}
```

Regenerate Go code: `bash scripts/generate_proto.sh`

### The interceptor chain — `cmd/grpc-auth/main.go`

```go
// ChainUnaryInterceptor executes A → B → handler (outermost first).
// It is gRPC's equivalent of net/http middleware chaining.
grpc.NewServer(
    grpc.ChainUnaryInterceptor(
        authgrpc.UnaryTraceInterceptor(tracer),  // 1st: extract traceparent, open span
        authgrpc.UnaryLogInterceptor(),           // 2nd: log method + gRPC status code
    ),
)
```

### Trace context over gRPC — `internal/grpc/server.go`

gRPC metadata = HTTP headers for gRPC. The W3C `traceparent` is injected by the
caller into gRPC metadata. To read it, you need a `TextMapCarrier` adapter:

```go
// metadataCarrier adapts metadata.MD to propagation.TextMapCarrier
// so the standard W3C propagator can read traceparent from gRPC metadata.
type metadataCarrier metadata.MD

func (mc metadataCarrier) Get(key string) string {
    vals := metadata.MD(mc).Get(key)
    if len(vals) == 0 { return "" }
    return vals[0]
}
func (mc metadataCarrier) Set(key, val string) { metadata.MD(mc).Set(key, val) }
func (mc metadataCarrier) Keys() []string       { /* ... */ }

// In UnaryTraceInterceptor:
if md, ok := metadata.FromIncomingContext(ctx); ok {
    ctx = otel.GetTextMapPropagator().Extract(ctx, metadataCarrier(md))
}
// Now ctx has the parent span from the caller → child span links correctly
```

### Why CheckToken returns `valid=false` instead of a gRPC error

A gRPC `status.Error(codes.Unauthenticated, ...)` means the RPC itself failed.
The client might retry it or treat it as a network error.

But "bad token" is a legitimate *answer* — the call succeeded, the answer is "no".
Returning `{valid: false, reason: "token expired"}` in the body lets the caller
distinguish "bad token" (handle gracefully) from "gRPC service down" (alert/retry).

Only `codes.InvalidArgument` is returned for truly malformed input (empty token).

### Test with grpcurl

```bash
# Server reflection lets grpcurl discover services without the .proto file
grpcurl -plaintext localhost:50051 list
# → auth.AuthService

# Get a real token first
TOKEN=$(curl -s -X POST localhost:8081/login \
  -d '{"username":"admin","password":"admin123"}' \
  | python3 -c "import sys,json; print(json.load(sys.stdin)['token'])")

# Call the gRPC service
grpcurl -plaintext \
  -d "{\"token\":\"$TOKEN\"}" \
  localhost:50051 auth.AuthService/CheckToken
# → { "valid": true, "username": "admin", "role": "admin" }

# Test with bad token
grpcurl -plaintext \
  -d '{"token":"not-a-valid-jwt"}' \
  localhost:50051 auth.AuthService/CheckToken
# → { "valid": false, "reason": "parsing token: ..." }
```

---

## 5. Running the Full Observability Stack

```bash
# 1. Start Collector + Jaeger + Prometheus
cd go-services/docker
docker compose -f docker-compose-otel.yml up -d

# 2. Start any Go service with OTLP endpoint
OTLP_ENDPOINT=localhost:4317 go run ./cmd/auth-service
OTLP_ENDPOINT=localhost:4317 go run ./cmd/grpc-auth
OTLP_ENDPOINT=localhost:4317 go run ./cmd/event-gateway
OTLP_ENDPOINT=localhost:4317 go run ./cmd/kpi-consumer  # needs KAFKA_BROKERS too

# 3. Make a request to generate spans
curl -X POST localhost:8081/login -d '{"username":"admin","password":"admin123"}'

# 4. Open Jaeger UI → search "auth-service" → click the trace
open http://localhost:16686

# 5. Open Prometheus → query kpi_consumer_messages_processed_total
open http://localhost:9090
```

---

## 6. Interview Q&A

**Q: What is the difference between a Trace and a Span?**

A Trace is the complete journey of one request — it has a single `trace_id` shared
across all services. A Span is one operation within that trace (e.g. "bcrypt.compare").
A trace is a tree of spans linked by `parent_id`. In Jaeger you click one trace and
see all spans as a waterfall.

---

**Q: Why BatchSpanProcessor instead of SimpleSpanProcessor?**

SimpleSpanProcessor sends one network call per span. At 1000 req/s that's 1000 calls/s
to the Collector — catastrophic. BatchSpanProcessor buffers spans in memory and flushes
every 5s (or 512 spans). This amortises network overhead across hundreds of spans per call.
Tradeoff: spans buffered for up to 5s are lost if the process crashes without calling
`tp.Shutdown()`. That's why every service defers the shutdown function.

---

**Q: Why use a Collector instead of exporting directly to Jaeger?**

Decoupling. Services only need to know one address (the Collector). If you want to add
a second backend (e.g. Datadog alongside Jaeger), or change sampling rate, or add
tail-based sampling — you reconfigure the Collector, zero service restarts. Without
the Collector you'd change every service's exporter config when switching backends.

---

**Q: Why pass r.Context() into tracer.Start() instead of context.Background()?**

`r.Context()` carries the current active span (the HTTP span otelhttp created). When you
call `tracer.Start(r.Context(), "bcrypt.compare")`, the SDK reads the parent span from
the context and sets `parent_id` automatically. If you used `context.Background()` the
span would have no parent and appear as an isolated root trace in Jaeger — completely
disconnected from the HTTP request. This is the most common OTEL mistake.

---

**Q: What is the metadataCarrier?**

The W3C propagator works with any carrier implementing `TextMapCarrier` (Get/Set/Keys).
HTTP uses `http.Header` which already implements this. But gRPC uses `metadata.MD`
(its own map type) which doesn't. `metadataCarrier` is a thin wrapper adapting
`metadata.MD` to `TextMapCarrier`. This lets the standard propagator extract
`traceparent` from gRPC metadata the same way it would from HTTP headers.

---

**Q: High cardinality — what does it mean and why does it matter in Prometheus?**

Cardinality = number of unique label value combinations. `node="AMF"` with 6 NF types
= 6 time series total = low cardinality, fine. `user_id="..."` with 1M users = 1M
time series = high cardinality, Prometheus OOMs. Rule: never label by user_id,
request_id, trace_id, IP address, or any value with unbounded possible values.

---

**Q: What does AlwaysSample mean? When would you NOT use it?**

AlwaysSample records 100% of requests. In dev/demo: fine. In production at 50k req/s
you'd generate millions of spans/s and saturate storage. Use `TraceIDRatioBased(0.01)`
for 1% head-based sampling, or configure tail-based sampling in the Collector (sample
based on whether the trace had an error — keeps all error traces, drops 99% of
successful ones).
