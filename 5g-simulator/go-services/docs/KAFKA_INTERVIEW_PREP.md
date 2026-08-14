# Kafka + Go Interview Prep

Written for someone who knows C++ well, knows basic Python, and has never
touched Kafka or a microservices pipeline. Every concept below is explained
using the actual code in this repo — `internal/events/kafka.go`,
`cmd/event-gateway/main.go`, `cmd/kpi-consumer/main.go` — not a generic
tutorial example. If an interviewer asks you to point at code, you can.

---

## Part 0: Go vs. JS vs. C++ — clearing up the confusion

You mentioned hearing "JS is very important" and being unsure why Kafka
"isn't JS" and why C++ "can't do the Golang situation." Short, direct
answers:

- **This job's JD requires Go, not JavaScript.** Re-read the JD you pasted:
  "2+ years in Go," "Strong knowledge and experience with Go concurrency,"
  "Build high-performance Kafka consumers/producers in Go." Node.js/TypeScript
  is listed under **"Nice to Have"** — the lowest tier, not a requirement.
  Whatever you heard about "JS is very important" is not what this specific
  JD is asking for. Don't let that anxiety redirect your prep time.
- **C++ "can" do everything Go does technically** — C++ has threads, mutexes,
  condition variables, and there are C++ Kafka clients (librdkafka). The
  reason companies use Go for this kind of service isn't capability, it's
  ergonomics: goroutines + channels make concurrent I/O-bound code (which is
  exactly what a Kafka consumer/producer is — waiting on network, not
  crunching numbers) much shorter and harder to get wrong than the
  equivalent `std::thread` + `std::mutex` + `std::condition_variable` C++
  code. You already know the C++ primitives (mutex, condvar, thread) — a
  goroutine is like a very cheap thread (starts at ~2KB stack, not ~8MB), and
  a channel is like a built-in, safe `std::queue` + condition variable
  bundled together. You are not learning a new *concept*, you're learning a
  lighter-weight *syntax* for a concept you already have solid intuition for
  from 8 years of C++.
- **Why not just use C++ for the Kafka side too?** You could — but the Go
  ecosystem's Kafka client libraries, HTTP frameworks, and Kubernetes tooling
  are more mature/idiomatic for this kind of "glue service that moves JSON
  between a queue and a database" work, which is most of what a telecom
  ingestion/KPI pipeline actually is. C++ still wins for the parts of this
  repo that need raw performance and precise binary protocol handling — your
  4g-simulator/5g-simulator C++ layer does APER-encoded S1AP/NGAP parsing,
  which is exactly the kind of job C++ is right for and Go would be a poor
  fit for. **This repo's own architecture is your answer**: C++ for
  protocol-level NF simulation, Go for the service layer wrapping it
  (event-gateway, control-api, auth-service, kpi-consumer). That split isn't
  arbitrary — it's "use the right tool for each job," and you can say exactly
  that in the interview.

---

## Part 1: Kafka concepts, explained via this repo's actual code

### Topic
A **topic** is a named, append-only log — think of it as a single, very long
text file that things get appended to, that many independent programs can
read from and re-read, and nobody can edit past entries in. This repo has
two topics: `5g-sim-events` (every `SimEvent` event-gateway publishes) and
`5g-sim-events-dlq` (events kpi-consumer gave up processing). See
`internal/config/config.go` — `KafkaTopic` / `KafkaDLQTopic`.

**C++ analogy**: not a `std::queue` (which empties as you pop) — closer to a
`std::vector` you only ever `push_back` onto, where multiple readers each
keep their own read-cursor (index) into the same vector.

### Partition
A topic is split into **partitions** — independent, ordered sub-logs. This
repo's Kafka is configured for 3 partitions per topic
(`KAFKA_NUM_PARTITIONS: 3` in `docker/docker-compose.kafka.yml`). Kafka only
guarantees order *within* a partition, never across the whole topic. That's
why `KafkaPublisher.Publish` (`internal/events/kafka.go`) picks a **key**
(SUPI, or Node if there's no SUPI) — Kafka hashes the key to decide which
partition a message goes to, so every event for the same subscriber always
lands on the same partition and is therefore always delivered to consumers
in the order it was sent. Events for *different* subscribers can land on
different partitions and be processed in parallel — that's the throughput
win.

**C++ analogy**: sharding a `std::unordered_map` across N buckets by
`hash(key) % N`, except Kafka does the ordering/durability/replay part for
you per-bucket.

### Producer
The thing that writes to a topic. In this repo: `KafkaPublisher` in
`internal/events/kafka.go`, used by `event-gateway`. It's a thin wrapper
around `kafka.Writer` (from `segmentio/kafka-go`).

### Consumer / Consumer Group
The thing that reads from a topic is a **consumer**. A **consumer group** is
a named set of consumer instances that *share* the work of reading a topic —
Kafka guarantees each partition is read by exactly one member of the group
at a time, so if you run 3 consumer processes in the same group against a
3-partition topic, each one gets roughly 1/3 of the traffic, automatically,
with zero coordination code you have to write. `cmd/kpi-consumer/main.go`
joins group `kpi-readers` (`KAFKA_GROUP_ID`). `k8s/kpi-consumer-deployment.yaml`
runs 2 replicas specifically so this load-splitting is observable, not just
theoretical.

**C++ analogy**: this is the piece C++ doesn't give you for free. In C++
you'd hand-roll a work-queue + thread pool + your own "which thread owns
which shard" bookkeeping. Kafka's consumer group protocol *is* that
bookkeeping, running across processes/machines instead of just threads in
one process.

### Offset
Each message in a partition has a sequential **offset** (0, 1, 2, ...) — its
position in that partition's log. A consumer group tracks, per partition,
"the last offset we've successfully processed" (the **committed offset**).
On restart, a consumer resumes from its group's last committed offset — it
doesn't need to remember anything itself, Kafka remembers for it. This is
also *why* at-least-once redelivery happens: if a consumer crashes after
processing a message but before its offset commit is written, the next
consumer to pick up that partition re-reads from the last committed offset —
including that already-processed message. See the idempotency comment in
`aggregator.ingest` in `cmd/kpi-consumer/main.go` for how this repo handles
that.

### DLQ (Dead-Letter Queue)
A DLQ is just another Kafka topic (`5g-sim-events-dlq` here) that you route
messages to when normal processing keeps failing, instead of endlessly
retrying or crashing. `processWithRetry` in `cmd/kpi-consumer/main.go`
retries a failed message 3 times (1s, 2s, 4s backoff), and only after all
three fail does it write the raw message to the DLQ topic and move on. The
reason this matters: Kafka delivers one partition's messages **in order, to
one consumer at a time**. If message #50 is malformed and you don't handle
it, you either crash (whole consumer dies) or get stuck retrying #50 forever
— either way, messages #51, #52, #53... never get processed, even though
they're perfectly fine. A DLQ lets you acknowledge and move past the one bad
message while keeping it around (not silently dropped) for someone to
inspect or replay later.

### Idempotency
"Idempotent" = doing something twice has the same effect as doing it once.
Because Kafka is at-least-once (see Offset above), your consumer *will*
occasionally see the same message twice. `aggregator.ingest` handles this by
tracking, per KPI window, which event IDs it has already counted
(`seenIDs map[string]bool`) — if the same event ID shows up again, it's
skipped rather than counted twice. Without this, a redelivered burst of
events after a consumer restart would silently inflate your KPI numbers
(e.g., a health percentage briefly reading over 100%, or an attempt count
that's higher than reality).

### Consumer Lag
The gap between "the latest offset available in a partition" and "the
offset this consumer group has actually processed." High/growing lag means
the consumer can't keep up with the producer — the #1 signal you'd watch in
production. `kpi-consumer` exposes this as `kpi_consumer_lag` (a Prometheus
gauge) on `/metrics`, read from `kafka.Reader.Stats().Lag`.

### Windowed Aggregation (tumbling window)
`aggregator.ingest` buckets each event by
`event.Timestamp.Truncate(a.windowSize)` — e.g., with a 60s window, an event
at `10:01:47` is bucketed into the window that started at `10:01:00`.
"Tumbling" means windows are fixed-size and don't overlap (as opposed to a
*sliding* window, where one event could belong to several overlapping
windows). A background ticker (`closeExpiredWindows`) checks every 5s for
windows whose end time has passed (plus a small grace period) and prints one
KPI line per window the first time it's seen as closed.

---

## Part 2: How to run it end to end

```bash
cd 5g-simulator/go-services

# 1. Start Kafka (single broker, KRaft mode, no ZooKeeper)
docker compose -f docker/docker-compose.kafka.yml up -d kafka

# 2. Run kpi-consumer (in one terminal)
KAFKA_BROKERS=localhost:9092 go run ./cmd/kpi-consumer

# 3. Run event-simulator to generate synthetic events (in another terminal)
#    — this stands in for event-gateway when you don't have the C++ NFs
#    running; it publishes one event/second straight to Kafka.
KAFKA_BROKERS=localhost:9092 go run ./cmd/event-simulator

# (Optional, once C++ NFs / NRF are running: run the real ingestion path
# instead of/alongside the simulator)
#    KAFKA_BROKERS=localhost:9092 go run ./cmd/event-gateway

# 4. Watch kpi-consumer's terminal for lines like:
#    [10:01:00] AMF health: 91% ok (11 events)
#    (first line appears ~60-65s after the first event, since it's a 60s
#    tumbling window plus a 3s grace period before it closes)

# 5. Check metrics
curl localhost:8084/metrics | grep kpi_consumer
```

Optional: `http://localhost:8090` runs `kafka-ui` (started alongside `kafka`
in the same compose file) — a web UI to browse the topic, partitions, and
consumer group lag visually while you're learning, instead of only trusting
the terminal output.

---

## Part 3: JD topics, answered in your own project's terms

Use these as talking points, not scripts to memorize word-for-word — say
them in your own words so they don't sound rehearsed.

**Go concurrency (goroutines, channels, sync patterns)**
"`event-gateway` runs two background goroutines — a log-tailer and an
NRF-poller — coordinated with `sync.WaitGroup` for clean shutdown and
`context.Context` for cancellation. The SSE streaming handler uses a
per-client buffered channel with a non-blocking send (`select` with
`default`), so one slow HTTP client can't stall event delivery to everyone
else — that's a fan-out pattern I can walk through line by line."

**Kafka integration**
"I added a Kafka producer to `event-gateway` and a consumer-group-based
service, `kpi-consumer`, that does windowed aggregation. I picked
`segmentio/kafka-go` over `sarama` for a simpler API surface. Partition key
is the subscriber ID so per-UE event order is preserved."

**Docker**
"Kafka runs in KRaft mode — one container, no separate ZooKeeper — which was
the right trade for a demo/dev setup. Each Go service has its own
multi-stage Dockerfile: build stage compiles a static binary, run stage is
just `alpine` + the binary running as non-root (`USER nobody`), keeping
images around 15MB."

**Kubernetes**
"`kpi-consumer`'s Deployment runs 2 replicas specifically to make Kafka's
consumer-group partition-splitting observable — kill one pod, the other
picks up its partitions automatically, no code of mine involved. Liveness
and readiness probes hit `/health` on all services."

**Observability instrumentation**
"`kpi-consumer` exposes real Prometheus metrics via `client_golang`:
messages processed, DLQ count, and consumer lag. I can point at the exact
`promauto.NewCounter` / `NewGauge` calls."

**Secure coding practices**
"Containers run as non-root (`USER nobody`). Secrets (JWT signing key) come
from a K8s Secret, not the ConfigMap, and there's a documented plan
(`docs/PRODUCTION_EVOLUTION_GO_LAYER.md`) for graduating to Vault. I can
also speak to what's *not* hardened yet — Kafka's listener is PLAINTEXT, no
mTLS — because I documented that gap rather than glossing over it."

**Performance profiling & optimization**
*(Honest gap — see Part 5.)* "I haven't profiled this specific pipeline with
`pprof` yet — that's a natural next step now that it's running. I have used
profiling tools in C++ (be ready to name what you've actually used —
Valgrind/perf/gprof — and describe one real case)."

**Distributed systems**
"The consumer-group rebalancing behavior *is* a distributed systems problem
solved for me by Kafka — I can describe what happens during a rebalance
(partitions get reassigned, in-flight processing on the losing consumer may
be redelivered to the new owner, which is exactly why idempotency matters)."

**Streaming systems**
"`kpi-consumer` is a minimal stream processor: continuous consumption,
stateful windowed aggregation, watermark-like grace period before closing a
window. I know this isn't Kafka Streams/Flink-scale (no checkpointed state,
in-memory only) and can speak to what upgrading that would take."

**Time-series data**
"Each `SimEvent` is timestamped and I bucket by tumbling time windows — the
next natural step (noted in the JD as 'time-series storage') would be
writing these aggregates to a TSDB like Prometheus's own storage, InfluxDB,
or TimescaleDB instead of just printing them."

**Telecom network data**
"Events model real 5G NF semantics — AMF/UDM/SMF/UPF/NRF/gNB node types and
TS 23.502 procedure names (registration, PDU session establishment). This
isn't a generic 'orders and payments' Kafka demo, it's telecom event
semantics end to end."

**Windowed aggregation & state handling**
"Tumbling 1-minute windows keyed by (NF, window start), in-memory state
protected by a mutex, closed by a ticker with a grace period for late
arrivals — I can describe tumbling vs. sliding windows and why I chose
tumbling here (KPIs per fixed time bucket, not a continuously-updating
rolling average)."

**Retry & DLQ strategy**
"3 retries with exponential backoff (1s/2s/4s), then DLQ. I can explain why
DLQ exists — head-of-line blocking prevention on an ordered partition — not
just that I added one."

**Idempotent processing**
"Dedup by event ID within each aggregation window, because Kafka's
at-least-once delivery means redelivery is expected, not exceptional. I can
walk through the exact double-counting bug this prevents."

---

## Part 4: Likely follow-up questions, answered honestly

**"How would you handle exactly-once semantics?"**
Kafka has transactional producers/consumers for this, but it adds real
complexity. Honest answer: "I used idempotent *consumption* (dedup by ID)
rather than Kafka transactions, because for a metrics/KPI use case,
occasional double-processing prevented by app-level dedup is simpler to
reason about than wiring up Kafka transactions, and gets you the same
correctness guarantee for this use case."

**"What happens if kpi-consumer falls behind?"**
"Consumer lag grows — visible in the `kpi_consumer_lag` metric. Kafka
retains messages regardless (default retention, not tied to consumer
speed), so it catches up rather than losing data, just later than
real-time. If lag kept growing unbounded, the fix is more consumer
replicas (bounded by partition count) or a faster per-message processing
path."

**"Why not just use the in-memory BoundedStore for everything?"**
"That's what event-gateway did *before* this change, and still does for the
live SSE view — Kafka doesn't replace it, it adds durability and a second,
independent consumer path (`kpi-consumer`) that doesn't have to live inside
event-gateway's process."

**"What would break at 10x the load?"**
"Single Kafka broker becomes the bottleneck/single point of failure first —
production needs 3+ brokers. Second: `kpi-consumer`'s single mutex around
all windows would start to contend; sharding the aggregator map by NF or
window would fix that."

**If you get asked something you genuinely don't know**: say so, plainly —
"I haven't worked with that yet, but here's how I'd figure it out" — and
pivot to something adjacent you *do* know. That reads as far more senior
than guessing.

---

## Part 5: What's real vs. what's roadmap — don't overclaim

Keep this straight before you walk in, because a confident wrong answer to a
follow-up is worse than an honest "that's on my roadmap, not built yet."

| Claim | Status |
|---|---|
| Go goroutines/channels/WaitGroup in event-gateway | **Real** — was already true before this session |
| K8s liveness/readiness probes | **Real** — already true before this session |
| Kafka producer in event-gateway | **Real** — built and verified this session |
| Kafka consumer group + windowed KPI aggregation | **Real** — built and verified this session |
| Retry + exponential backoff + DLQ | **Real** — built and verified this session |
| Idempotent consumption | **Real** — built and verified this session |
| Prometheus metrics (real client library) | **Real, in kpi-consumer only** — control-api still uses a hand-rolled text handler |
| Grafana dashboards for this Kafka pipeline | **Not built** — no Grafana config wired to `kpi_consumer_*` metrics yet |
| Kubernetes HPA | **Not present** in this repo's k8s manifests |
| Multi-broker Kafka, TLS/SASL, schema registry | **Not built** — documented as gaps in `PRODUCTION_EVOLUTION_GO_LAYER.md`, don't claim these |
| pprof / performance profiling on this pipeline | **Not done** — be honest if asked for a specific profiling story |
