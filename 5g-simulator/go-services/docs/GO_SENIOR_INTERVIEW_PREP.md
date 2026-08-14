# Go Senior Backend Engineer — Interview Prep Guide

> **Who this is for:** You have 8+ years of C++ telecom (AMF, MME, UPF, IMS).
> You've implemented gRPC, Kafka, Docker/K8s, and OTEL in the go-services codebase
> here — but you want to *understand* what you built well enough to explain it
> confidently and defend it under interview pressure.
>
> Every section: simple explanation → ASCII diagram → your code → 4G/5G connection
> → top interview Q&A.

---

## Contents

1. [The Big Picture — What Are Microservices?](#1-the-big-picture)
2. [Go Fundamentals — Goroutines, Channels, Context, Interfaces](#2-go-fundamentals)
3. [gRPC — Typed Contracts Between Services](#3-grpc)
4. [Kafka — The Message Bus That Never Loses Your Data](#4-kafka)
5. [Docker & Kubernetes — Running Your Services at Scale](#5-docker--kubernetes)
6. [Observability — Traces, Metrics, Logs](#6-observability)
7. [Debugging Scenarios — When Production Breaks](#7-debugging-scenarios)
8. [Full Interview Q&A Bank](#8-interview-qa-bank)

---

## 1. The Big Picture

### What Is a Microservice? (And Why Do Companies Use Them?)

Imagine you're building a 5G core. Instead of one giant binary that does
everything — authentication, session management, charging, policy — 3GPP split
it into separate Network Functions: AMF, SMF, UDM, PCF, UPF.

Each NF:
- Does ONE thing well
- Has a clean API to the outside world (N1, N2, N3, N4...)
- Can be scaled independently (more AMF pods when registration spikes)
- Can crash without taking down the whole system

Microservices is the same idea applied to web backend software.

```
MONOLITH (the old way)           MICROSERVICES (the modern way)
─────────────────────────        ──────────────────────────────

  ┌──────────────────────┐         ┌───────────┐  ┌───────────┐
  │                      │         │  auth-    │  │  event-   │
  │  auth + events +     │         │  service  │  │  gateway  │
  │  kafka + metrics +   │         │  :8080    │  │  :8081    │
  │  everything in one   │         └─────┬─────┘  └─────┬─────┘
  │  giant process       │               │               │
  │                      │         ┌─────┴──────────────┴─────┐
  └──────────────────────┘         │         Kafka Bus         │
                                   └─────┬──────────────┬──────┘
  One crash = total outage               │              │
  One deploy = full restart       ┌──────┴────┐  ┌──────┴────┐
  One team = bottleneck           │  kpi-     │  │  grpc-    │
                                  │  consumer │  │  auth     │
                                  │  :9090    │  │  :50051   │
                                  └───────────┘  └───────────┘
```

**Your 4G/5G angle:** In Samsung you worked on AMF. The AMF doesn't do billing
(that's CHF), doesn't do policy (PCF), doesn't own subscriber data (UDM). You
LIVED microservices — you just called them Network Functions. The principles are
identical. Say exactly this in the interview.

### Your Codebase — Four Services, Each Owning One Domain

```
go-services/
├── cmd/
│   ├── auth-service/    ← owns identity: login, JWT issue, bcrypt verify
│   ├── event-gateway/   ← owns event streaming: SSE + log watching + NRF polling
│   ├── grpc-auth/       ← owns service-to-service token check (gRPC)
│   └── kpi-consumer/    ← owns metrics: consumes Kafka, exposes /metrics
│
├── internal/
│   ├── auth/            ← shared JWT/bcrypt library (no service owns this alone)
│   ├── events/          ← Kafka publisher + local bounded event store
│   ├── grpc/            ← gRPC server + interceptors (proto-generated types)
│   └── telemetry/       ← OTEL TracerProvider + Prometheus bridge (shared)
```

**Interview answer when asked "describe your microservices architecture":**
> "We have four services. auth-service owns user identity — JWT tokens, bcrypt
> password verification. grpc-auth provides a centralized token introspection
> endpoint over gRPC so other services don't duplicate JWT parsing. event-gateway
> aggregates simulator log lines and NRF changes, streams them to clients over
> SSE, and publishes to Kafka. kpi-consumer reads from Kafka and exposes
> Prometheus metrics. All four are instrumented with OpenTelemetry — traces,
> metrics — and run in Docker containers orchestrated by Kubernetes."

---

## 2. Go Fundamentals

### 2.1 Goroutines — Lightweight Threads

**What they are (simple):** A goroutine is a function that runs concurrently with
other goroutines. You just put `go` in front of any function call. Go's runtime
manages thousands of goroutines on a handful of OS threads — much cheaper than
creating an actual OS thread.

```
OS Thread = a truck (heavy, expensive, one per CPU)
Goroutine = a package on the truck (lightweight, many fit on one truck)
```

```go
// Without goroutine — sequential, blocks here until done
watchLogs()       // ← must finish before nrfPoller starts

// With goroutines — both run concurrently
go watchLogs()    // ← starts and immediately returns
go nrfPoller()    // ← also starts, both run simultaneously
```

**Your code — event-gateway starts two background goroutines:**
```
cmd/event-gateway/main.go

  ctx, cancel := signal.NotifyContext(...)
  go logWatcher(ctx, pub, cfg)     ← polls log files every 500ms
  go nrfPoller(ctx, pub, cfg)      ← queries NRF every 5s
  // main goroutine blocks waiting for OS signal (Ctrl+C)
  <-ctx.Done()
  cancel()
  // goroutines see ctx cancelled, clean up, exit
```

**4G/5G angle:** In C++ AMF you likely had POSIX threads (`pthread_create`) for
handling UE context processing in parallel. Goroutines are the same concept,
just 10x cheaper — you can have one per UE (subscriber) if needed.

**Interview Q: What's the difference between a goroutine and a thread?**
> "A goroutine is multiplexed onto OS threads by Go's M:N scheduler. Creating a
> goroutine costs ~2KB of stack vs ~8MB for a thread. You can run a million
> goroutines on a laptop. Threads are managed by the OS; goroutines by the Go
> runtime. The trade-off: you have to manage goroutine lifecycle yourself — use
> context cancellation to signal shutdown."

---

### 2.2 Channels — Safe Data Passing Between Goroutines

**What they are (simple):** A channel is a pipe between goroutines. One goroutine
puts data in; another takes data out. No shared memory, no mutexes needed.

```
Goroutine A                  Channel               Goroutine B
───────────             ──────────────────        ───────────
  publish(event)  ──→  [ev1][ev2][ev3]   ──→  consume(event)

  "writes into pipe"      "buffered queue"        "reads from pipe"
```

```go
// Unbuffered: sender blocks until receiver is ready
ch := make(chan Event)

// Buffered: sender can put up to 100 items before blocking
ch := make(chan Event, 100)

// Send
ch <- event

// Receive
event := <-ch

// Close (signals "no more data")
close(ch)
```

**Your code — BoundedStore uses a channel pattern internally:**
The events.BoundedStore in `internal/events/` holds the last N events and
notifies subscribers. The FanOutPublisher publishes to both the local store
AND Kafka simultaneously — the Kafka write path is the durable channel.

**Common pattern — fan-out with channels:**
```
         ┌─── ch1 ──→ subscriber 1 (SSE client)
Producer ─┤─── ch2 ──→ subscriber 2 (SSE client)
         └─── ch3 ──→ subscriber 3 (SSE client)
```

**Interview Q: When would you use a channel vs a mutex?**
> "Channels for transferring ownership of data between goroutines — producer/
> consumer pipelines, fan-out patterns. Mutexes for shared state that multiple
> goroutines READ AND WRITE — like a map of active connections or a counter.
> Go's mantra: 'Don't communicate by sharing memory; share memory by
> communicating.' But for simple atomic counters, `sync/atomic` or `atomic.Int64`
> is cheaper than either."

---

### 2.3 Context — Cancellation, Deadlines, Timeouts

**What it is (simple):** `context.Context` is a value you pass through every
function call that can be cancelled. When you cancel the root context (e.g., on
shutdown or timeout), ALL functions that received that context can detect it and
stop cleanly.

```
main()
  │
  ├── ctx, cancel = signal.NotifyContext(background, SIGTERM)
  │
  ├── go logWatcher(ctx)    ← sees ctx.Done() when SIGTERM arrives
  ├── go nrfPoller(ctx)     ← same
  │
  └── http.Server.Shutdown(ctx)  ← graceful drain
```

**Timeout pattern — every external call gets a deadline:**
```go
// In kafka.go — 3-second timeout on Kafka write
ctx, cancel := context.WithTimeout(ctx, 3*time.Second)
defer cancel()
err := w.WriteMessages(ctx, kafka.Message{...})
// If Kafka is slow, WriteMessages returns after 3s instead of hanging forever
```

**4G/5G angle:** In PFCP (N4), every session modification has a timer. If the
response doesn't arrive, you timeout and retry. `context.WithTimeout` is the
Go equivalent of your N4 request timer.

**Interview Q: What happens if you forget to call cancel()?**
> "Memory leak. The context and everything attached to it (deadline timer, cancel
> channel) stays alive until the parent context is cancelled. In long-running
> servers this accumulates. `defer cancel()` right after `WithTimeout` or
> `WithCancel` is the idiomatic fix."

---

### 2.4 Interfaces — Duck Typing Done Right

**What they are (simple):** An interface in Go is a contract. If a type has
all the methods an interface requires, it automatically satisfies that interface.
No `implements` keyword needed.

```go
// Interface: "anything that can publish an event"
type Publisher interface {
    Publish(ctx context.Context, e SimEvent) error
}

// KafkaPublisher — satisfies Publisher (has the Publish method)
type KafkaPublisher struct { writer *kafka.Writer }
func (k *KafkaPublisher) Publish(ctx context.Context, e SimEvent) error {...}

// FanOutPublisher — also satisfies Publisher
type FanOutPublisher struct { local Publisher; remote Publisher }
func (f *FanOutPublisher) Publish(ctx context.Context, e SimEvent) error {...}

// Code that uses Publisher doesn't care which one it gets:
func startGateway(pub events.Publisher) {
    // works with KafkaPublisher OR FanOutPublisher OR a test mock
}
```

**Why this matters for interviews:** Interviewers love asking how you'd test a
service that writes to Kafka. Answer: "The service takes a `Publisher` interface.
In tests I pass a mock Publisher. In production I pass KafkaPublisher. No code
change needed."

**4G/5G angle:** In 3GPP you have abstract interfaces like N4 (UPF talks to SMF)
— the interface spec says what messages are exchanged, not how the vendor
implements it. Go interfaces are the same: the contract matters, not the
implementation.

---

### 2.5 Error Handling — Explicit Over Magic

Go has no exceptions. Every function that can fail returns an error as its last
return value. You check it explicitly.

```go
token, err := auth.Issue(secret, username, role)
if err != nil {
    // handle it — log, return, retry, whatever
    return fmt.Errorf("issuing token for %s: %w", username, err)
}
// token is safe to use here
```

**`%w` (wrapping):** Lets you add context while keeping the original error
inspectable with `errors.Is()` / `errors.As()`.

```go
// Downstream returns: "timeout"
// Wrapped up the stack: "kafka publish: issuing token: timeout"
// errors.Is(err, context.DeadlineExceeded) → true, even wrapped 3 levels deep
```

**Interview Q: How do you handle errors in a concurrent pipeline?**
> "I use an errgroup. `errgroup.Group` runs goroutines and collects the first
> error. The context it returns is cancelled when any goroutine fails. All other
> goroutines see the cancellation and stop. It's the right tool when you have
> N parallel operations that should all succeed or all stop."

---

## 3. gRPC

### What Is gRPC? (Start From Zero)

Imagine service A needs to call a function in service B. The naive way is HTTP:
- A serializes arguments to JSON
- Sends an HTTP POST
- B deserializes JSON
- Runs the function
- Serializes result back to JSON
- A deserializes the result

**gRPC is a better version of this:**
- Arguments are defined in a `.proto` file (typed, not loose JSON)
- Protobuf serialization (binary, 3-10x smaller than JSON)
- HTTP/2 under the hood (multiplexed — many calls on one TCP connection)
- Code generation: `protoc` generates client AND server stubs in any language

```
                    .proto file (the contract)
                    ┌───────────────────────────┐
                    │ service AuthService {      │
                    │   rpc CheckToken(          │
                    │     AuthCheckRequest)      │
                    │   returns (AuthCheckResp); │
                    │ }                          │
                    └───────────┬───────────────┘
                               protoc
                    ┌──────────┴──────────────────┐
                    │                             │
             authpb/auth.pb.go            authpb/auth_grpc.pb.go
             (message structs)            (client + server interfaces)
```

### The Call Flow

```
  Caller (event-gateway)              grpc-auth :50051
  ──────────────────────             ─────────────────
  conn, _ := grpc.Dial(":50051")
  client := authpb.NewAuthServiceClient(conn)
  req := &authpb.AuthCheckRequest{Token: jwt}
  resp, err := client.CheckToken(ctx, req)
                    │
                    │  [HTTP/2 frame, binary protobuf]
                    │  ────────────────────────────────→
                    │                                  UnaryTraceInterceptor
                    │                                  ↓ extract traceparent
                    │                                  UnaryLogInterceptor
                    │                                  ↓ log method name
                    │                                  CheckToken handler
                    │                                  ↓ auth.Verify(jwt)
                    │  ←──────────────────────────────
  resp.Valid = true
  resp.Username = "alice"
  resp.Role = "viewer"
```

### Your Code — What Was Actually Built

**`internal/grpc/server.go`** — the gRPC server:

```go
// AuthServer holds the secret key and tracer
type AuthServer struct {
    authpb.UnimplementedAuthServiceServer
    jwtSecret string
    tracer    trace.Tracer
}

// CheckToken is the RPC handler — validates JWT, returns claims
func (s *AuthServer) CheckToken(ctx context.Context, req *authpb.AuthCheckRequest) (*authpb.AuthCheckResponse, error) {
    ctx, span := s.tracer.Start(ctx, "auth.verify")  // nested OTEL span
    defer span.End()

    claims, err := auth.Verify(s.jwtSecret, req.Token)
    if err != nil {
        return &authpb.AuthCheckResponse{Valid: false, Reason: err.Error()}, nil
    }
    return &authpb.AuthCheckResponse{Valid: true, Username: claims.Username}, nil
}
```

**Interceptors** (= gRPC middleware, like HTTP middleware but for RPC):

```
Incoming gRPC call
        │
        ▼
UnaryTraceInterceptor   ← extracts W3C traceparent from gRPC metadata,
        │                 starts a server span so this call appears in Jaeger
        ▼
UnaryLogInterceptor     ← logs: method=CheckToken code=OK
        │
        ▼
CheckToken handler      ← your actual business logic
        │
        ▼  (return path, unwinding the chain)
UnaryLogInterceptor     ← records code
UnaryTraceInterceptor   ← ends span
```

**4G/5G angle:** gRPC is essentially what PFCP is — a binary RPC protocol between
two network functions. In PFCP (N4), the SMF sends `Session_Establishment_Request`
and the UPF responds with `Session_Establishment_Response`. In gRPC, the caller
sends `AuthCheckRequest` and the server responds with `AuthCheckResponse`. Both
use:
- Binary encoding (protobuf vs PFCP TLVs)
- Typed messages (proto schema vs 3GPP IEs)
- Request-response pattern
- Code-generated stubs (protoc vs ASN.1 compiler)

The only difference is gRPC is standardized for general use; PFCP is a telecom
standard. You built BOTH — mention this.

### gRPC vs REST — When to Use Which

```
                gRPC                         REST/HTTP
                ─────                        ─────────
Schema:         .proto (strict, typed)       OpenAPI/JSON (loose)
Encoding:       protobuf (binary, small)     JSON (text, human-readable)
Streaming:      yes (server-stream,          only with SSE/WebSocket
                client-stream, bidirectional)
Browser calls:  needs grpc-web proxy         native fetch()
Service-to-svc: perfect fit                  works, more overhead
Error codes:    gRPC codes (NOT_FOUND, etc.) HTTP status codes
Code gen:       automatic (protoc)           manual or openapi-gen
```

**When you chose gRPC:** For grpc-auth (service-to-service token introspection).
Internal calls where you control both ends, latency matters, schema must be strict.

**When you kept REST:** For auth-service (login endpoint). Browsers and curl need
to call it — gRPC doesn't work natively in browsers.

---

### Interview Q&A — gRPC

**Q: What is protobuf and why use it over JSON?**
> "Protocol Buffers is a binary serialization format with a schema (`.proto`
> file). It's 3-10x smaller than JSON and 5-10x faster to parse because it uses
> field numbers (integers) instead of field names (strings). The trade-off: not
> human-readable — you need the `.proto` to decode a binary payload. For
> internal service-to-service calls this is fine. For public APIs where
> debuggability matters, JSON wins."

**Q: How do you handle backward compatibility in protobuf?**
> "You never reuse a field number and never remove a field — just mark it
> `reserved`. Add new optional fields with new numbers. Old clients ignore
> unknown fields; new clients handle missing fields via defaults. This gives you
> rolling deployments: update the server first, then clients — both old and new
> clients work during the transition."

**Q: What is a gRPC interceptor and have you used one?**
> "An interceptor is gRPC's middleware — a function that wraps every RPC call.
> In grpc-auth I implemented two: `UnaryTraceInterceptor` which extracts the
> W3C traceparent header from gRPC metadata and starts an OTEL span (so the
> gRPC call appears as a child span in Jaeger tracing), and `UnaryLogInterceptor`
> which logs every method name and gRPC status code. They're chained via
> `grpc.ChainUnaryInterceptor`. The pattern is identical to HTTP middleware."

**Q: gRPC deadline propagation — what is it?**
> "When a caller sets a deadline on a gRPC call, that deadline is sent to the
> server in the gRPC metadata. If the server is slow, the client can cancel the
> call and the server should detect the context cancellation and stop processing.
> This prevents a slow downstream from cascading into an upstream timeout. In
> our grpc-auth calls we pass the request context which carries any deadline set
> by the HTTP handler — so if an HTTP client sets a 5s timeout, the gRPC call
> to grpc-auth will also be bounded by that 5s."

---

## 4. Kafka

### What Is Kafka? Start From the Problem.

**Problem:** auth-service wants to tell kpi-consumer "a login happened." How?

Option 1: auth-service calls kpi-consumer's HTTP endpoint directly.
- What if kpi-consumer is down? auth-service fails.
- What if kpi-consumer is slow? auth-service waits.
- What if you add another consumer (alerting service)? auth-service must know
  about every consumer and call each one.

Option 2: A message bus. auth-service puts a message on the bus and forgets.
kpi-consumer reads from the bus when it's ready. Alerting service also reads
from the same bus. auth-service doesn't know or care who's consuming.

**Kafka is that message bus.**

```
PRODUCER                    KAFKA                     CONSUMERS
────────                 ──────────                  ──────────

auth-service             Topic: "events"
  │                      ┌─────────────┐
  │  "login: alice"  →   │ partition 0 │ → [msg1][msg3][msg5]...
  │  "login: bob"    →   │ partition 1 │ → [msg2][msg4][msg6]...
  │  "logout: alice" →   │ partition 0 │
  │                      └─────────────┘
  │                                         kpi-consumer (group A)
  event-gateway                             reads partition 0 + 1
    │  "NRF change"   →   Topic: "nrf"
                                            alerting-service (group B)
                                            reads partition 0 + 1 independently
                                            (its own offset pointer — doesn't
                                            affect kpi-consumer's progress)
```

### Core Concepts — Explained Simply

#### Topic
A named feed of messages. Like a TV channel — producers broadcast to it,
consumers tune in. Example: `events`, `nrf-changes`, `billing-records`.

#### Partition
A topic is split into N ordered, append-only logs called partitions.
Think of it as N parallel lanes on a highway.
- Messages within one partition are STRICTLY ORDERED
- Messages across partitions are NOT ordered
- More partitions = more parallelism = more throughput

```
Topic "events" with 3 partitions:
────────────────────────────────
partition 0:  [msg0][msg3][msg6][msg9]...     ← UE: alice (SUPI=...001)
partition 1:  [msg1][msg4][msg7][msg10]...    ← UE: bob   (SUPI=...002)
partition 2:  [msg2][msg5][msg8][msg11]...    ← UE: carol (SUPI=...003)

Each partition is one ordered log, written by ONE broker, read by ONE consumer
instance at a time.
```

**Your code — partition key = SUPI:**
```go
// internal/events/kafka.go
w := kafka.NewWriter(kafka.WriterConfig{
    Balancer: &kafka.Hash{},  // hash the message key → pick partition
})
// When publishing:
kafka.Message{
    Key:   []byte(supi),   // SUPI = subscriber permanent identifier
    Value: eventBytes,
}
// All events for the same SUPI always go to the same partition.
// This guarantees ORDER per subscriber (same as TEID per UE in GTP-U).
```

**4G/5G angle:** In GTP-U, each UE's packets are identified by TEID. You NEVER
mix packets from different TEIDs in the same tunnel (that would corrupt the UE's
flow). Kafka partition keys work exactly the same way: same key = same partition
= ordered delivery for that subscriber. When you built this with SUPI as the key,
you applied your telecom instinct correctly.

#### Offset
The position of a message within a partition. Starts at 0, increments by 1.
Each consumer group maintains its own offset — "I've read up to offset 47."

```
partition 0:
  offset:   0     1     2     3     4     5
  message: [m0]  [m1]  [m2]  [m3]  [m4]  [m5]
                                     ↑
                              kpi-consumer committed here
                              (will read m4 next on restart)
```

Offset is how Kafka lets consumers restart without losing messages. The consumer
tracks where it left off and resumes from there.

#### Consumer Group
A set of consumer instances that share the work of reading a topic.
Kafka assigns each partition to exactly ONE consumer in the group.

```
Topic "events" — 4 partitions
Consumer Group "kpi-workers" — 2 instances

  partition 0  ──→  kpi-consumer-pod-0
  partition 1  ──→  kpi-consumer-pod-0
  partition 2  ──→  kpi-consumer-pod-1
  partition 3  ──→  kpi-consumer-pod-1

Scale to 4 pods? Each pod gets 1 partition. (Max useful pods = num partitions)
Scale to 8 pods? 4 pods are idle — no partition to read.
```

**Rebalance:** When a pod dies or a new pod joins, Kafka redistributes partitions
among the remaining consumers. This is called a rebalance. During rebalance,
consumption pauses briefly — a real production concern.

#### At-Least-Once Delivery
Kafka's default guarantee. A message is delivered at least once, but possibly
more than once (if the consumer crashes after processing but before committing
the offset).

```
Consumer reads message → processes it → CRASH (before commit)
Consumer restarts       → reads SAME message again (offset not committed)
Consumer processes it   → commits offset
```

Result: message processed twice. For billing or counters, that's a bug.
Fix: make processing IDEMPOTENT (same message processed twice = same result).

```go
// Idempotent counter: use a set of seen IDs
seenEvents := map[string]bool{}

func process(event Event) {
    if seenEvents[event.ID] {
        return // already processed, skip
    }
    seenEvents[event.ID] = true
    incrementCounter(event)
}

// Or in a database: INSERT ... ON CONFLICT DO NOTHING
```

**4G/5G angle:** In PFCP, if the UPF doesn't respond to a Session_Establishment
request, the SMF retries. The UPF might have received and processed the first
request but the response was lost. The UPF must be idempotent — processing the
same session setup twice should not create two sessions. Your instinct for
handling retransmissions in telecom maps directly here.

### What You Built in kafka.go

```go
// KafkaPublisher — writes to Kafka, keyed by SUPI for partition ordering
type KafkaPublisher struct {
    writer *kafka.Writer
}

func (k *KafkaPublisher) Publish(ctx context.Context, e SimEvent) error {
    ctx, cancel := context.WithTimeout(ctx, 3*time.Second) // don't hang forever
    defer cancel()

    data, _ := json.Marshal(e)
    return k.writer.WriteMessages(ctx, kafka.Message{
        Key:   []byte(e.SUPI),  // partition key: same SUPI = same partition
        Value: data,
    })
}

// FanOutPublisher — publishes to BOTH local cache AND Kafka
// local: for SSE subscribers already connected (zero latency)
// Kafka: for durability, for kpi-consumer to read later
type FanOutPublisher struct {
    local  Publisher  // BoundedStore (in-memory)
    remote Publisher  // KafkaPublisher (durable)
}

func (f *FanOutPublisher) Publish(ctx context.Context, e SimEvent) error {
    _ = f.local.Publish(ctx, e)   // don't fail the chain if local fails
    return f.remote.Publish(ctx, e)
}
```

---

### Interview Q&A — Kafka

**Q: What is a Kafka partition and why does it matter?**
> "A partition is an ordered, append-only log — one lane of a Kafka topic. You
> choose how many partitions a topic has when you create it. More partitions =
> more parallelism: N consumers in a group can read in parallel, one per partition.
> The key architectural point is that messages with the same key always go to the
> same partition, giving you per-key ordering. In our code, events keyed by SUPI
> (subscriber ID) always land in the same partition — so we get per-subscriber
> event ordering at no extra cost."

**Q: What is at-least-once delivery and how do you handle duplicates?**
> "At-least-once means Kafka guarantees you'll receive a message at least once,
> but if a consumer crashes after processing and before committing its offset,
> it'll receive the message again on restart. To handle this, you make processing
> idempotent: if you've already processed a message ID, you skip it. In our
> kpi-consumer, incrementing a Prometheus counter is already idempotent — a
> duplicate event just adds 1 twice, which in aggregated metrics over time is
> negligible. For critical financial data you'd use a database with
> `ON CONFLICT DO NOTHING` keyed by the Kafka message offset."

**Q: What happens during a Kafka consumer rebalance?**
> "When a consumer pod is added or removed, Kafka triggers a rebalance: it
> temporarily pauses consumption, reassigns partitions among active consumers,
> then resumes. During a rebalance, you might see a brief spike in consumer lag.
> In production I monitor consumer lag — if lag consistently grows, I scale up
> consumer pods. But I never scale beyond the partition count, because extra
> pods get no partition assigned and sit idle."

**Q: What is consumer lag and how do you monitor it?**
> "Consumer lag = (latest Kafka offset) minus (consumer's committed offset). If
> lag is 0, the consumer is caught up. If lag is 10,000 and growing, the consumer
> is falling behind — maybe it's slow, or there's a processing error causing
> retries. I expose lag as a Prometheus gauge and alert when it exceeds a
> threshold. In our go-services setup, the kpi-consumer exposes Kafka metrics
> via the OTEL/Prometheus bridge."

**Q: Kafka vs RabbitMQ — when would you use each?**
> "Kafka stores messages durably on disk and lets any consumer group read from
> any point in time — it's a log, not a queue. Best for event streaming, audit
> logs, fan-out to multiple independent consumers. RabbitMQ is a traditional
> message queue — once consumed, the message is gone. Better for task queues
> where you want exactly one consumer to process each job. For our use case —
> multiple services consuming the same event stream — Kafka is the right choice."

---

## 5. Docker & Kubernetes

### Docker — What It Solves (Simply)

**Problem:** "It works on my Mac but not in prod."

The reason: your Mac has Python 3.12, prod has Python 3.8. Your Mac has a
certain version of a library, prod has a different one. Different OS, different
filesystem, different environment variables.

**Docker's solution:** Package your application AND all its dependencies into a
single image — a snapshot of the exact environment it needs. Run that image
anywhere and it behaves identically.

```
Dockerfile (recipe)                    Docker Image (built result)
───────────────────                    ──────────────────────────
FROM golang:1.22-alpine                one self-contained filesystem:
WORKDIR /app                           ├── go binary (statically linked)
COPY go.mod go.sum ./                  ├── ca-certificates
RUN go mod download                    ├── /etc/passwd
COPY . .                               └── nothing else
RUN go build -o auth-service ./cmd/auth-service

# Final image: scratch (empty)
FROM scratch
COPY --from=0 /app/auth-service /
EXPOSE 8080
CMD ["/auth-service"]
```

**Key concepts:**
- **Image:** The snapshot (like a VM image, but smaller — no OS kernel)
- **Container:** A running instance of an image (like a process with an isolated filesystem)
- **Registry:** Where images are stored (Docker Hub, GCR, ECR)
- **Layer:** Each `RUN` or `COPY` in Dockerfile creates a layer. Layers are cached.

**Multi-stage build (what you have):** First stage compiles Go (needs Go toolchain,
~300MB). Second stage is `scratch` (empty) — just copies the compiled binary.
Final image: ~10MB.

### Kubernetes — Running Containers at Scale

**Problem:** You have 4 microservices. Each needs to run multiple copies for
reliability. They need to find each other. When a pod crashes, something needs
to restart it. When traffic spikes, you need to scale up.

Kubernetes (K8s) handles all of this.

```
                    Kubernetes Cluster
    ────────────────────────────────────────────────────
    │                                                  │
    │   ┌─────────────┐      ┌─────────────┐          │
    │   │  Node 1     │      │  Node 2     │          │
    │   │             │      │             │          │
    │   │ [auth-svc]  │      │ [auth-svc]  │          │
    │   │ [grpc-auth] │      │ [event-gw]  │          │
    │   │ [kpi-cons]  │      │ [kpi-cons]  │          │
    │   └─────────────┘      └─────────────┘          │
    │                                                  │
    │   ┌──────────────────────────────────────────┐  │
    │   │            Control Plane                  │  │
    │   │  API Server  ← kubectl commands           │  │
    │   │  Scheduler   ← "where to put this pod?"  │  │
    │   │  Controller  ← "is the desired state met?"│  │
    │   └──────────────────────────────────────────┘  │
    ────────────────────────────────────────────────────
```

**Core K8s objects:**

**Pod:** One or more containers sharing network and storage. The smallest unit.
Pods are ephemeral — they die and are replaced.

**Deployment:** "Run 3 replicas of auth-service. If one dies, start a new one.
When I update the image, do a rolling update (replace one pod at a time, zero
downtime)."

**Service:** A stable network address for a set of Pods. Pods come and go (their
IPs change), but the Service IP stays fixed. Other services talk to the Service,
not individual Pods.

```
auth-service Service (ClusterIP: 10.96.0.5:8080)
         │
         ├──→ auth-service Pod (10.244.1.3:8080)  ← load balanced
         ├──→ auth-service Pod (10.244.2.7:8080)  ← round-robin
         └──→ auth-service Pod (10.244.1.9:8080)
```

**4G/5G angle:** K8s Service = NRF (Network Repository Function). NRF is the
service discovery layer in 5G Core — AMF asks NRF "where is SMF?" and NRF
returns the SMF's address. K8s Service does the same: the Pod IP changes but
the Service DNS name stays stable. This is exactly the `nrfPoller` goroutine
in event-gateway — it polls the NRF to detect NF registration changes. K8s
just does this automatically for all services.

**ConfigMap and Secret:**
- ConfigMap: non-sensitive config (Kafka broker address, log level)
- Secret: sensitive values (JWT secret, TLS cert, DB password), base64-encoded
  and mounted as env vars or files in the Pod

**Horizontal Pod Autoscaler (HPA):**
```
HPA: "When auth-service CPU > 70%, scale from 2 → 4 replicas"
     "When CPU drops < 30%, scale back to 2"
```

**What you have in go-services:**
- `docker-compose.yml`: Local dev — starts all 4 services + Kafka + OTEL collector
- `k8s/`: Kubernetes manifests — Deployments, Services, ConfigMaps for each service
- `docker-compose-otel.yml`: Adds Jaeger and Prometheus to the local stack

---

### Interview Q&A — Docker & Kubernetes

**Q: What is the difference between a Docker image and a container?**
> "An image is an immutable snapshot — the compiled binary, dependencies, and
> filesystem packaged together. A container is a running instance of that image,
> with its own isolated process, network, and filesystem. Same image can run as
> 10 containers simultaneously. If a container crashes, you start a new one from
> the same image — the image is unchanged."

**Q: What is a Kubernetes liveness probe vs readiness probe?**
> "Liveness: 'Is this container still alive?' — Kubernetes restarts the container
> if this fails. Use for detecting deadlocks or infinite loops where the process
> is running but not functioning. Readiness: 'Is this container ready to receive
> traffic?' — Kubernetes removes the pod from the load balancer if this fails.
> Use during startup (the service needs 5s to warm up) or during heavy load
> (the service is temporarily overloaded). A pod can be alive (liveness passes)
> but not ready (readiness fails) — it won't crash-loop but also won't get traffic."

**Q: How does a pod in Kubernetes find another service?**
> "Via Kubernetes DNS. Every Service gets a DNS name:
> `<service-name>.<namespace>.svc.cluster.local`. For example,
> `auth-service.default.svc.cluster.local:8080`. The kube-dns pod resolves this
> to the Service's ClusterIP, and kube-proxy load-balances across healthy pods.
> You just use the DNS name in your config — you don't hardcode pod IPs."

**Q: What happens to in-flight requests during a rolling update?**
> "Kubernetes does rolling updates by default: it starts a new pod with the new
> image, waits for its readiness probe to pass, then terminates one old pod.
> During the termination, it sends SIGTERM to the old pod and gives it
> `terminationGracePeriodSeconds` (default 30s) to drain in-flight requests.
> In our auth-service, we handle SIGTERM via `signal.NotifyContext` and call
> `srv.Shutdown(10s)` — this stops accepting new connections, waits up to 10s
> for in-flight handlers to complete, then exits. Without graceful shutdown,
> in-flight requests get dropped."

---

## 6. Observability

### The Three Pillars

```
                    ┌─────────────────────────────────────┐
                    │         OBSERVABILITY                │
                    │                                      │
                    │   TRACES      METRICS     LOGS       │
                    │   ─────────   ────────    ─────      │
                    │   "what      "are we     "what       │
                    │    happened   healthy?"   exactly    │
                    │    to this                happened?" │
                    │    request?"                         │
                    │                                      │
                    │   Jaeger      Prometheus  slog/zap   │
                    └─────────────────────────────────────┘
```

### Traces — Following One Request Through Multiple Services

**What it is (simple):** When a login request comes in, it touches auth-service,
then maybe grpc-auth, and publishes an event to Kafka. A trace is the complete
picture of that request's journey — which services it touched, how long each
took, where it was slow.

```
HTTP POST /login   (total: 47ms)
  │
  ├─ auth.bcrypt_compare         (43ms) ← this is the bottleneck!
  ├─ auth.jwt_issue              (1ms)
  └─ grpc.CheckToken → grpc-auth (2ms)
        └─ auth.verify           (1ms)
```

**How it works (spans and trace ID):**

Every request gets a random trace ID (e.g., `4bf92f3577b34da6`). Every unit of
work gets a span with a start/end time. Spans know their parent span.
All spans with the same trace ID form the waterfall you see in Jaeger.

```
Trace ID: 4bf92f3577b34da6
                                                      time →
  auth-service: /login     ───────────────────────────────────── 47ms
    auth.bcrypt_compare       ──────────────────────────── 43ms
    auth.jwt_issue                                         ─ 1ms
    grpc.CheckToken                                           ── 2ms
      grpc-auth: /auth.AuthService/CheckToken               ── 2ms
        grpc-auth: auth.verify                              ─ 1ms
```

**Your code — how traces propagate between services:**

```go
// auth-service (HTTP handler)
// otelhttp.NewHandler wraps the mux → creates root span for every HTTP request

// When auth-service calls grpc-auth:
// 1. otelhttp middleware injects traceparent into outgoing request context
// 2. grpc metadata carries: "traceparent: 00-4bf92f35...-abc123-01"
// 3. grpc-auth's UnaryTraceInterceptor extracts it:
if md, ok := metadata.FromIncomingContext(ctx); ok {
    ctx = otel.GetTextMapPropagator().Extract(ctx, metadataCarrier(md))
}
// 4. Starts a CHILD span with same trace ID → waterfall view in Jaeger
ctx, span := tracer.Start(ctx, info.FullMethod, trace.WithSpanKind(trace.SpanKindServer))
```

**4G/5G angle:** A trace is like following a UE's call setup across multiple
network functions: the UE sends Attach Request → MME → HSS → SGW → PGW. Each
hop is a "span." The trace ID is like the UE's IMSI — it identifies whose journey
this is. OTEL traces just make this automatic and visual.

### Metrics — Is the System Healthy Right Now?

**What they are (simple):** Numbers measured over time. "How many requests per
second? What's the P99 latency? How many errors in the last 5 minutes?"

**Three types you need to know:**

```
COUNTER:     monotonically increasing. Never goes down.
             Use for: total requests, total errors, total events published.
             Example: event_gateway_events_published_total

GAUGE:       goes up AND down. Current snapshot.
             Use for: active connections, queue depth, memory usage.
             Example: event_gateway_subscribers_active

HISTOGRAM:   records distribution of values. Enables P50, P95, P99.
             Use for: request latency, message processing time.
             Example: http_request_duration_seconds{route="/login"}
```

**Your code — OTEL metrics in event-gateway:**
```go
eventsPublished, _ := meter.Int64Counter(
    "event_gateway.events_published",
    metric.WithUnit("{event}"),
)
// Record on each publish:
eventsPublished.Add(ctx, 1, metric.WithAttributes(
    attribute.String("node", event.Node),
))

subscriberCount, _ := meter.Int64UpDownCounter(
    "event_gateway.subscribers_active",
)
// +1 when SSE client connects, -1 when disconnects:
subscriberCount.Add(ctx, 1)
// ...
subscriberCount.Add(ctx, -1)
```

**Prometheus bridge (how OTEL metrics become Prometheus metrics):**
```
OTEL SDK → PrometheusExporter → prometheus.DefaultRegisterer
                                          │
                                          └─ /metrics endpoint
                                                │
                                          Prometheus scrapes every 15s
                                                │
                                          Grafana dashboards + alerts
```

### Interview Q&A — Observability

**Q: What is distributed tracing and why do you need it in microservices?**
> "In a monolith, a slow request shows up in a single profiler. In microservices,
> a slow request might be slow because service A is waiting for service B which
> is waiting for a database. Without distributed tracing, you have no idea which
> hop is slow — you're debugging 4 services separately trying to correlate logs
> by timestamp. A distributed trace carries a trace ID through every service hop,
> giving you a unified waterfall view of the entire request path. In our setup,
> the trace ID propagates via the W3C `traceparent` header through HTTP and gRPC
> metadata — I set this up in the gRPC interceptor."

**Q: What's the difference between a counter and a gauge? Give examples.**
> "A counter only goes up — it counts things that happen. Example: total requests
> served, total errors. You query the rate of change (requests per second) not
> the absolute value. A gauge is a snapshot of current state — it can go up or
> down. Example: current active database connections, current Kafka consumer lag,
> current in-flight requests. In event-gateway, events_published is a counter
> (events keep accumulating), subscribers_active is a gauge (clients connect and
> disconnect)."

**Q: What is a P99 latency and why does it matter more than average?**
> "P99 means 99% of requests are faster than this value. Average latency hides
> tail latency — if 99% of requests take 10ms and 1% take 5 seconds, the average
> might look fine at 60ms. But that 1% is 10,000 users/hour having a terrible
> experience. Histograms let you compute any percentile. In production I alert on
> P99, not average. Typical SLOs: P99 < 500ms, P50 < 50ms."

---

## 7. Debugging Scenarios

These are the "tell me about a production issue" questions. Use the STAR format
(Situation, Task, Action, Result) and be specific about what tools you used.

---

### Scenario 1: Latency Spike in auth-service

**Symptom:** Grafana alert: `http_server_duration_ms{route="/login"}` P99 > 3s.
Auth-service is responding slowly.

**Debugging approach:**

```
Step 1: Look at traces in Jaeger
        Filter by /login route, sort by duration
        → bcrypt_compare span = 2.8s   ← suspect

Step 2: Check what changed
        git log --since="30 minutes ago"
        → no recent deploys

Step 3: Look at system metrics
        kubectl top pods -n default
        → auth-service-pod: CPU 95%
        kubectl top nodes
        → node1: CPU 98%

Step 4: Check concurrent requests
        Prometheus query: rate(http_server_requests_total[1m])
        → request rate doubled in last 20 min (traffic spike)

Root cause: bcrypt work factor = 12 (intentional: makes password brute-force slow)
            Takes ~300ms per request normally.
            Under high load, CPU is saturated, bcrypt takes 2-3s.

Fix options:
  Short term: HPA — scale auth-service from 2 → 6 pods
  Long term: cache bcrypt results briefly (login rate-limiting),
             or reduce work factor for non-sensitive accounts
```

**What to say in interview:**
> "We had a P99 latency spike on our login endpoint. I pulled the Jaeger trace for
> a slow request and immediately saw bcrypt_compare taking 2.8 seconds. Normally
> it's 300ms — bcrypt's work factor is intentionally high to make brute-force
> expensive. I checked Kubernetes — the pods were CPU-saturated because a traffic
> spike doubled the request rate. Short-term fix: scale horizontally via HPA.
> Long-term: added a login rate limiter to prevent burst saturation."

---

### Scenario 2: Kafka Consumer Lag Growing

**Symptom:** `kafka_consumer_lag` metric growing steadily. kpi-consumer is
falling behind.

**Debugging approach:**

```
Step 1: Check consumer lag metric
        kafka_consumer_lag{topic="events",partition="0"} = 50,000 and growing

Step 2: Check consumer logs
        kubectl logs -f deployment/kpi-consumer
        → "WriteMessages: context deadline exceeded" (3s timeout hits repeatedly)
        → Retrying...

Step 3: The consumer is spending all its time retrying Kafka writes
        Wait — kpi-consumer READS from Kafka, it doesn't write
        Something downstream of consume is slow

Step 4: Check what kpi-consumer does with messages
        → It writes metrics to a time-series DB
        → Time-series DB is down (PVC full)

Step 5: Fix: expand PVC, restart DB
        → Lag drains within minutes as consumer catches up

Lesson learned: Add Kafka lag alert. Alert on DB disk usage (80% → page).
```

**What to say in interview:**
> "Our Kafka consumer lag started growing — the kpi-consumer was falling behind.
> I checked the consumer logs and saw repeated timeout errors on database writes.
> The consumer was processing messages but failing to commit the results, so it
> kept retrying the same messages while new ones piled up. Root cause: the
> backing storage for the metrics database was full. Fix: expand the volume.
> I also added two alerts: one for Kafka consumer lag > 1,000 (early warning),
> one for disk usage > 80%."

---

### Scenario 3: Pod Not Receiving Traffic

**Symptom:** grpc-auth service deployed. Callers getting connection refused.
kubectl get pods shows pods Running. But no traffic reaches them.

**Debugging approach:**

```
Step 1: Check pod status
        kubectl get pods -n default
        NAME                      READY     STATUS
        grpc-auth-7d4f9c-abc12    0/1       Running    ← 0/1 = not ready!

Step 2: Describe the pod
        kubectl describe pod grpc-auth-7d4f9c-abc12
        → Readiness probe failed: connection refused on port 50051

Step 3: Check the readiness probe config
        kubectl get deployment grpc-auth -o yaml
        readinessProbe:
          grpcProbe:
            port: 50051
          initialDelaySeconds: 5  ← starts checking after 5s

Step 4: Check gRPC server logs
        kubectl logs grpc-auth-7d4f9c-abc12
        → "listen tcp :50051: bind: address already in use"

Step 5: Root cause: two containers in same pod both trying port 50051
        (config mistake: duplicate port in Pod spec)

Step 6: Fix: correct the Deployment YAML, re-apply
```

**What to say in interview:**
> "Pods showed Running but READY was 0/1 — the readiness probe was failing.
> I described the pod and saw connection refused on the gRPC port. The logs
> showed a bind error — address already in use. A misconfiguration had two
> containers in the same pod specification trying to listen on the same port.
> Fixed the Deployment YAML and reapplied. Key lesson: `kubectl describe pod`
> gives you the probe failure reason immediately — that's always my first stop
> when pods are Running but not ready."

---

### Scenario 4: SSE Clients Getting Duplicate Events

**Symptom:** event-gateway SSE subscribers see the same event twice.

**Debugging approach:**

```
Step 1: Check event-gateway replicas
        kubectl get pods -l app=event-gateway
        → 2 replicas

Step 2: SSE is stateful — the client connects to ONE pod.
        If the client reconnects (network blip), it might connect to the other pod.
        The other pod's BoundedStore has overlapping events.

Step 3: Check the SSE reconnection logic in the client
        → Client sends Last-Event-ID header on reconnect
        → event-gateway should skip events already sent
        → But if the reconnect lands on a different pod, the new pod
          doesn't know what the old pod sent.

Root cause: no sticky sessions (session affinity) configured.
            Client reconnects to a different pod, gets duplicate events.

Fix options:
  Option A: Kubernetes Service with sessionAffinity: ClientIP
            Same client IP always routes to same pod.
  Option B: All pods read from Kafka (durable, ordered) instead of in-memory store.
            Client sends Last-Event-ID → pod reads from Kafka offset after that ID.
            This works across pod restarts and rebalances.

We went with Option A (simpler, right for our scale).
Long-term: Option B is the production-correct solution.
```

---

## 8. Interview Q&A Bank

### Go Language

**Q: What are goroutines and how are they different from OS threads?**
> "Goroutines are lightweight concurrent functions managed by Go's runtime
> scheduler. A goroutine starts with 2-4KB of stack vs 1-8MB for an OS thread.
> The Go scheduler multiplexes M goroutines onto N OS threads (M:N threading).
> You can run millions of goroutines on a normal laptop. OS threads are managed
> by the kernel — goroutines are managed by Go's cooperative/preemptive scheduler
> in userspace. The practical difference: you write `go myFunc()` and don't think
> about threads at all."

**Q: How does Go's garbage collector affect latency?**
> "Go has a concurrent, tri-color mark-and-sweep GC. Since Go 1.14, it targets
> < 1ms STW (stop-the-world) pauses. For most web services this is fine. For
> ultra-low-latency paths (like our DPDK UPF which needs sub-microsecond), Go GC
> is too unpredictable — that's why we kept the UPF in C++. For the auth-service
> and gRPC services, Go GC is completely fine. If you need to reduce GC pressure
> in Go: use sync.Pool for frequently allocated objects, prefer value types over
> pointers where possible, use slices with pre-allocated capacity."

**Q: What is the select statement in Go?**
> "Select is like a switch for channels — it waits for whichever case is ready
> first. Used for: multiplexing multiple channels, implementing timeouts with
> `time.After`, and detecting context cancellation."
```go
select {
case event := <-events:
    process(event)
case <-ctx.Done():    // context cancelled (shutdown / timeout)
    return
case <-time.After(30*time.Second):  // fallback timeout
    log.Warn("no events in 30s")
}
```

**Q: What is sync.Mutex vs sync.RWMutex?**
> "sync.Mutex: one goroutine at a time can hold it, regardless of whether it's
> reading or writing. sync.RWMutex: multiple goroutines can hold the read lock
> simultaneously (parallel reads); only one goroutine can hold the write lock
> (exclusive). Use RWMutex when reads are frequent and writes are rare — like
> a route table, a config cache, or an in-memory session store. In our
> BoundedStore, subscriber state reads are frequent (SSE heartbeats), writes are
> rare (new subscriber connects) — RWMutex is the right choice."

**Q: What is sync.Pool and when do you use it?**
> "sync.Pool is a cache of allocated-but-unused objects. Instead of allocating a
> new buffer for every HTTP request and then garbage-collecting it, you get one
> from the pool, use it, and put it back. This reduces GC pressure. Classic use:
> `bytes.Buffer` pools for JSON encoding, `[]byte` pools for network I/O buffers.
> Important: the pool can be drained at any GC cycle — objects in the pool are
> not guaranteed to be there. Use it for performance, not for synchronization."

**Q: What is a defer and when is it useful?**
> "defer schedules a function call to run when the surrounding function returns,
> regardless of how it returns (normal return, panic, early return). It's Go's
> RAII. Most common uses: `defer mutex.Unlock()` right after `mutex.Lock()` —
> you can't forget to unlock. `defer span.End()` right after `span := tracer.Start()`.
> `defer cancel()` right after `ctx, cancel := context.WithTimeout()`. The golden
> rule: if you acquire something that needs to be released, defer the release
> immediately."

---

### Microservices Architecture

**Q: What is the 12-factor app and have you followed it?**
> "12-factor is a methodology for building portable, scalable SaaS apps. The ones
> most relevant to our work: Factor 3 (Config): configuration in environment
> variables, not hardcoded — we pass JWT_SECRET, KAFKA_BROKER via env. Factor 4
> (Backing services): treat Kafka, DB, etc. as attached resources by URL — our
> services don't have Kafka addresses baked in. Factor 8 (Concurrency): scale
> out via process model — we scale by adding pods. Factor 9 (Disposability):
> fast startup, graceful shutdown — we handle SIGTERM with a 10s drain window."

**Q: How do services communicate in your architecture? Synchronous vs async?**
> "We use both. Synchronous gRPC for token introspection — the caller needs an
> immediate answer (valid/invalid) before it can proceed. Asynchronous Kafka for
> event publishing — auth-service publishes 'user logged in' to Kafka and moves
> on; kpi-consumer processes it whenever it's ready. The rule: use sync when the
> caller needs the result NOW; use async when the caller just needs to notify and
> doesn't need a response."

**Q: How do you handle a service that's temporarily down?**
> "For synchronous calls (gRPC): circuit breaker pattern. After N consecutive
> failures, stop calling the downstream and return a cached or degraded response.
> After a cooldown period, try again. Libraries: `go-resiliency`, `sony/gobreaker`.
> For async (Kafka): the producer just publishes to Kafka — Kafka buffers it.
> The consumer is down? No problem — it catches up when it restarts. Kafka's
> durability decouples producer availability from consumer availability. This is
> the key advantage of async messaging."

---

### Distributed Systems

**Q: What is the CAP theorem?**
> "CAP: a distributed system can guarantee at most two of: Consistency (all nodes
> see the same data), Availability (every request gets a response), Partition
> tolerance (system works even if nodes can't talk to each other). Network
> partitions happen — you can't avoid P. So the real choice is: during a
> partition, do you want CP (consistent but might be unavailable) or AP (always
> available but might return stale data)? Kafka is CP by default — a partition
> election can briefly make a topic unwritable. Most web APIs are AP — they'd
> rather return slightly stale data than return an error."

**Q: What is eventual consistency?**
> "Eventual consistency means: if no new writes happen, all replicas will
> EVENTUALLY converge to the same value. Not immediately — there's a propagation
> delay. In our architecture: auth-service writes a 'login' event to Kafka.
> kpi-consumer reads it and increments a counter. If you query the counter 1ms
> after login, you might not see the increment yet (Kafka consumer lag).
> That's fine for our use case — we're not a bank. For a bank balance, you'd
> want strong consistency (synchronous write with acknowledgment before returning
> to user)."

**Q: What is a distributed lock and when do you need one?**
> "A distributed lock ensures only one instance of your service performs an
> operation at a time — even when multiple pods are running. Example: a cron job
> that sends daily summary emails. With 3 pods, all 3 might try to send at the
> same time. A Redis-based lock (SET key NX EX 60) lets only one pod 'win.'
> The loser sees the key is taken and skips. In Go: `redsync` library provides
> a Redis-based distributed mutex. For our services, we don't currently need
> this — each service type has a single clear owner."

---

### System Design (Common Interview Questions)

**Q: Design a URL shortener (like bit.ly)**
> Key points to cover: write service generates short code (base62 of auto-increment
> ID), stores in DB (`short_code → long_url`). Read service (high volume) does
> hash lookup, returns 301 redirect. Cache reads with Redis (hot URLs). Scale
> write with DB partitioning by short_code. Scale read with read replicas +
> Redis.

**Q: How would you rate-limit an API endpoint?**
> "Token bucket algorithm: each user gets a bucket with N tokens. Each request
> costs 1 token. Tokens refill at rate R/second. If the bucket is empty, reject
> the request (429). In Go: `golang.org/x/time/rate` implements token bucket.
> For distributed rate limiting (multiple pods): store the bucket in Redis with
> a Lua script to atomically check-and-decrement. For our auth-service login
> endpoint: rate-limit by IP (prevents brute-force) AND by username (prevents
> enumeration)."

**Q: How do you design for zero-downtime deployments?**
> "Four things: (1) Graceful shutdown: catch SIGTERM, stop accepting new requests,
> drain in-flight requests, then exit. We do this in all four services with
> signal.NotifyContext + srv.Shutdown(10s). (2) Readiness probes: K8s won't send
> traffic to a new pod until its readiness probe passes — new pod warms up first.
> (3) Rolling updates: replace one pod at a time, never taking all replicas down
> at once. (4) Backward-compatible deploys: API and DB schema changes must be
> compatible with the old version while rollout is in progress — add columns
> as nullable, version APIs with /v2 prefix."

---

### Your Personal Story — Connecting 4G/5G to Go Backend

**When they ask "walk me through a complex system you built":**

> "At Samsung I worked on the AMF — the Access and Mobility Management Function.
> The AMF is essentially the 'session manager' for every connected UE. It
> handles NAS signaling from the UE, coordinates with the SMF for session setup,
> and maintains state for potentially millions of concurrent subscribers.
>
> The interesting engineering challenge was: every UE has state (registration
> state, security context, PDU session list), and that state needs to be
> consistent even when AMF pods are restarted. We had to think carefully about
> which state could be recomputed vs which had to be persisted.
>
> When I started building Go microservices for this simulator project, I
> recognized the same patterns: auth-service is like the AMF's security context
> — it issues tokens (like the AMF issues NAS security mode commands). The
> Kafka topic keyed by SUPI mirrors how the AMF routes NAS messages per IMSI —
> same subscriber, same processing lane, strict ordering guaranteed.
>
> The OTEL tracing we added maps directly to the call flows in 3GPP spec diagrams
> — a UE registration procedure touches AMF→UDM→SMF. In microservices, a login
> request touches auth-service→grpc-auth. The trace shows the waterfall either
> way."

---

## Appendix: Quick-Reference Cheat Sheet

```
CONCEPT            WHAT IT IS                    YOUR CODE           4G/5G PARALLEL
────────────────── ──────────────────────────── ─────────────────── ──────────────────
goroutine          lightweight concurrent fn    logWatcher go func  pthread in AMF
channel            pipe between goroutines      events chan          N2 message queue
context            cancellation propagation     ctx.Done()          N4 timer
interface          duck-typed contract          Publisher interface  3GPP interface spec
gRPC               typed binary RPC             grpc-auth :50051    PFCP (N4)
protobuf           binary schema'd encoding     authpb/*.pb.go      PFCP TLVs / ASN.1
interceptor        RPC middleware               UnaryTraceInterceptor S1AP layer
Kafka topic        named message feed           "events" topic      N3 GTP-U path
Kafka partition    ordered lane in topic        keyed by SUPI       TEID per UE tunnel
consumer group     shared consumers             kpi-consumer pods   multiple UPFs
at-least-once      may deliver twice            FanOutPublisher     PFCP retransmit
offset             consumer position in log     committed per CG    sequence number
Docker image       self-contained app snapshot  Dockerfile multi-stage  VNF image
K8s Pod            running container(s)         auth-service pod    VNF instance
K8s Service        stable network address       ClusterIP :8080     NRF registration
K8s Deployment     desired-state manager        3 replicas          NF redundancy
HPA                auto-scale on CPU/memory     scale to 6 pods     load-based NF spawn
trace              request journey across svcs  Jaeger waterfall    call flow diagram
span               one unit of work in trace    auth.bcrypt span    procedure step
counter            monotone metric              events_published    Tx packet count
gauge              current-state metric         subscribers_active  active bearers
histogram          distribution / percentiles   P99 login latency   OM measurement
```

---

*This guide covers what was actually built in `go-services/` and connects it to
your 8 years of C++ telecom experience. Every section has a code reference you
can open, an explanation you can give in plain English, and a 4G/5G anchor so
you can make it personal.*
