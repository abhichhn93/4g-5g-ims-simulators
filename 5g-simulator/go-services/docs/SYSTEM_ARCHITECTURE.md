# System Architecture: How C++, Go, Kafka, and Kubernetes Actually Fit Together

> **Who this is for:** you already know the C++ side well — sockets, a
> thread/threading situation somewhere, UDM/AMF talking to each other over
> some protocol. This doc is for the part that's fuzzy: where does Go
> actually sit, how do the C++ NFs *really* find each other (spoiler: it's
> not Go), and where does Kafka and Kubernetes fit on top of all that.

---

## Part 0: The one-paragraph mental model

**The C++ layer is the entire 5G core — it works completely on its own,
with no Go involved, and always has.** AMF, UDM, SMF, UPF, gNB, NRF all
talk to each other directly over their own sockets, using their own
NRF-based discovery mechanism (explained below) — written entirely in C++,
years before Go entered this repo. **Go does not sit *inside* that call
flow at all.** It sits *beside* it, as a separate operations layer: it
watches the C++ layer from the outside (tailing its log files, polling its
NRF over HTTP as a read-only observer), gives humans/CI a REST API to
start/stop the simulation, and — now — publishes what it observes onto
Kafka so a second, independent process can compute rolling KPIs from it.
If you deleted the entire Go layer right now, the C++ 5G core would keep
running exactly as before. That one fact resolves most of the confusion.

---

## Part 1: The big picture, as one diagram

```
┌───────────────────────────── C++ NF PLANE (the real 5G core) ──────────────────────────────┐
│                                                                                               │
│   gNB ──SCTP/TCP socket──▶ AMF ──HTTP/JSON──▶ UDM         AMF ──HTTP/JSON──▶ SMF ──▶ UPF     │
│  :sim   (register/attach)   │38412              │29503      │                  │29502   │8805 │
│                              │                                │                              │
│                              │   every NF registers itself with, and discovers peers via:    │
│                              └───────────────────────▶  NRF :29510  ◀────────────────────────┘
│                                  Nnrf_NFManagement_NFRegister (PUT)  /  Nnrf_NFDiscovery_Search (GET)
│                                          — all C++, sockets + HTTP text built by hand —       │
└──────────────────────────────────────────────┬──────────────────────────┬───────────────────┘
                                                 │                          │
                              tails log files    │                          │ polls, read-only,
                          (SIM_LOG_DIR/g5_*.log) │                          │ every 5s (GET /nf-instances)
┌────────────────────────────────────────────────▼──────────────────────────▼──────────────────┐
│                    GO CONTROL / OBSERVABILITY PLANE  (sits BESIDE, never inside)              │
│                                                                                                  │
│  auth-service :8081        control-api :8082 ──exec.Command──▶ launches the C++ binaries above │
│  (JWT login/roles)          (start/stop scenario, /metrics)                                    │
│                                                                                                  │
│  event-gateway :8083                                                                            │
│     logWatcher goroutine ─┐                                                                     │
│     nrfPoller  goroutine ─┴─▶ Publish(SimEvent) ──▶ [BoundedStore: local SSE, /events]          │
│                                                  └─▶ [KafkaPublisher: durable, replayable]       │
└──────────────────────────────────────────────────────────────────┬─────────────────────────────┘
                                                                      │  topic: 5g-sim-events
                                                            ┌─────────▼─────────┐
                                                            │   Kafka broker      │  KRaft, 1 node,
                                                            │   3 partitions      │  docker-compose.kafka.yml
                                                            └─────────┬─────────┘
                                                          consumer group "kpi-readers"
                                                                      │
                                                            ┌─────────▼──────────────┐
                                                            │  kpi-consumer :8084      │
                                                            │  tumbling-window KPIs,   │
                                                            │  retry+DLQ, idempotent   │
                                                            └──────────────────────────┘
```

Read it as three horizontal bands, top to bottom: **C++ does the actual 5G
protocol work. Go watches C++ and exposes controls. Kafka+kpi-consumer
turn what Go observed into rolling metrics.** Nothing in the top band
depends on anything below it — that arrow direction never reverses.

---

## Part 2: How do the C++ NFs actually find each other?

This is the part you half-remembered as "some Golang feature" — it isn't.
It's a C++ helper, `nrfclient::registerSelf()` / `nrfclient::discover()` in
[`src/common/nrf_client.h`](../../src/common/nrf_client.h), used by every
NF (UDM, AMF, and friends). It implements the real 3GPP Service-Based
Architecture (SBA) pattern:

```
1. UDM starts up
   UDM ──PUT /nnrf-nfm/v1/nf-instances/{id}──▶ NRF
        body: {nfInstanceId, nfType:"UDM", host:"udm-sim", port:29503}
   "Here I am, remember me."

2. AMF starts up, needs to reach UDM, but doesn't hardcode udm-sim:29503
   AMF ──GET /nnrf-disc/v1/nf-instances?target-nf-type=UDM──▶ NRF
   NRF ──▶ {"host":"udm-sim","port":29503}
   "Where's UDM right now? ... Over there."

3. AMF now opens its OWN direct socket straight to udm-sim:29503
   AMF ══TCP socket, HTTP/JSON on the wire══▶ UDM
   (NRF is out of the picture again until someone needs to look someone
   else up — it's a directory service, not a proxy in the data path)
```

Concretely, in code (`nrf_client.h`):
- `registerSelf(...)` — builds a JSON body by hand, opens a `Socket`
  (your own socket wrapper, same primitive you already know from raw
  sockets), sends a hand-built `PUT` request, retries up to 10× 1s apart
  in case NRF hasn't started yet (Docker/K8s start order isn't guaranteed).
- `discover(...)` — same socket pattern, a `GET` with a query string,
  parses `host`/`port` out of the JSON response text.

**None of this is Go.** It's `std::string`, hand-rolled HTTP text, and your
own `Socket` class — the exact "sockets + some protocol" mental model you
already had, just applied to a JSON-over-HTTP protocol (called the SBI —
Service-Based Interface — in real 3GPP specs) instead of a binary one.
Go never touches this exchange, doesn't participate in it, and isn't
required for it to work — it's the same mechanism whether or not the Go
layer is even running.

**Where does the "threading situation" fit here?** In this sim, most NFs
(AMF, UDM, gNB) run a simple blocking-socket loop — one connection handled
at a time, no thread pool. The one place true multithreading (a real
`thread_pool.h`, in C++) shows up is `src/upf_fastpath/` — the
performance-oriented UPF datapath, where line-rate packet forwarding
actually needs concurrent workers. That's a C++ detail, unrelated to Go.

---

## Part 3: So what does Go actually *do*, and at what layer?

Go is a **sidecar operations layer**, not a protocol participant. Three
services, three jobs:

| Service | Job | Talks to C++ how |
|---|---|---|
| `event-gateway` | Turn C++ activity into a live event stream (SSE) + Kafka feed | Reads log files C++ already writes; polls NRF's `GET /nf-instances` (same endpoint AMF/UDM use for discovery, but event-gateway only ever reads it, never registers/discovers on behalf of a real NF) |
| `control-api` | Let a human/CI start or stop the whole C++ simulation | `exec.Command("./nrf_sim")` etc. — literally runs the same binaries you'd run by hand from a terminal |
| `auth-service` | Gate the above two behind login/JWT roles | Doesn't talk to C++ at all — pure Go, issues/verifies tokens |

So concretely: **event-gateway's `nrfPoller` goroutine calls the exact same
NRF HTTP endpoint (`GET /nnrf-disc/v1/nf-instances`) that AMF's C++
`discover()` calls** — but for a completely different purpose. AMF calls it
to get an address it's about to *connect to*. `nrfPoller` calls it every 5
seconds purely to notice "a new NF showed up" or "one disappeared," and
turns that into a `SimEvent` for the dashboard/Kafka. It's read-only
observation, not participation — see
[`cmd/event-gateway/main.go`](../cmd/event-gateway/main.go) function
`nrfPoller` / `fetchNFIDs`.

**Why Go for this instead of more C++?** Not because Go can do something
C++ can't (it can't — no SCTP, no ASN.1 tooling) — because this specific
job (HTTP JSON APIs, concurrent log-tailing, a REST control surface) is
*shorter and safer* in Go. `net/http` + goroutines gives you a production
HTTP server in ~20 lines; the equivalent thread-safe C++ HTTP server is
hundreds. This is exactly the split real 5G core vendors use: **C/C++ (or
Rust) for the protocol/data plane, Go for the SBI/management plane** — see
`docs/GOLANG_ARCHITECTURE.md` section 5 for the full "what stays in C++
forever" list.

---

## Part 4: Where Kafka fits — one more hop past Go, still outside the C++ loop

Kafka sits **downstream of Go, not between Go and C++.** event-gateway
still talks to C++ exactly as in Part 3 (log tail + NRF poll); the only
change Kafka introduces is *what event-gateway does with what it
observes*:

```go
// cmd/event-gateway/main.go, main()
var pub events.Publisher = store              // always: powers /events, /events/stream
if cfg.KafkaBrokers != "" {
    pub = &events.FanOutPublisher{Local: store, Kafka: kp}   // ALSO ship to Kafka
}
go logWatcher(ctx, cfg, pub)   // same goroutine as before — doesn't know/care Kafka exists
go nrfPoller(ctx, cfg, pub)
```

`logWatcher` and `nrfPoller` are unchanged from before Kafka existed —
they just call `pub.Publish(event)` and have no idea whether that's going
to memory, Kafka, or both. That's the `Publisher` interface
(`internal/events/model.go`) doing its job: the producers of events never
know or care about the transport.

On the other end, **`kpi-consumer` is a completely separate OS
process/pod** — it never talks to C++ at all, never talks to event-gateway
directly either. It only knows about Kafka: it joins consumer group
`kpi-readers`, reads whatever JSON events show up on topic
`5g-sim-events`, and computes a rolling health percentage per NF over
60-second tumbling windows. See `docs/KAFKA_INTERVIEW_PREP.md` for exactly
how that windowing/retry/DLQ/idempotency works.

**Why put Kafka in the middle at all, instead of kpi-consumer reading
straight from event-gateway?** Decoupling: kpi-consumer can crash, restart,
or fall behind for minutes, and it just resumes from its last committed
Kafka offset — event-gateway and the live SSE view are completely
unaffected either way. Without Kafka, you'd have to build that
buffering/replay logic yourself.

---

## Part 5: Kubernetes — two different discovery mechanisms stacked on top of each other

This repo has **two separate `k8s/` directories**, and understanding why
clarifies a subtlety worth having ready for an interview:

```
5g-simulator/k8s/                    5g-simulator/go-services/k8s/
  nrf-deployment.yaml + Service        auth-service-deployment.yaml
  amf-deployment.yaml + Service        control-api-deployment.yaml
  udm-deployment.yaml + Service        event-gateway-deployment.yaml
  smf-deployment.yaml, upf, gnb        kpi-consumer-deployment.yaml (2 replicas)
  = the C++ NF pods                    = the Go service pods
```

Each C++ NF Deployment gets a K8s `Service` (e.g. `g5-nrf-svc`), which
gives it a **stable DNS name inside the cluster** — pods come and go,
restart, get rescheduled to different nodes, but `g5-nrf-svc` always
resolves to *a* healthy NRF pod. Look at `k8s/amf-deployment.yaml`:

```yaml
env:
  - name: NRF_HOST
    value: g5-nrf-svc      # <- Kubernetes Service DNS name, not an IP
```

Now here's the subtlety: **this K8s DNS name is a *second, independent*
discovery layer sitting underneath the NRF discovery from Part 2.**

```
Layer 2 (network-level, Kubernetes):
   "Where is *a* NRF pod, physically, right now?"
   → answered by K8s Service DNS: g5-nrf-svc resolves to a live NRF pod IP

Layer 1 (application-level, 3GPP SBA, still C++, still nrf_client.h):
   "Which NF instance provides UDM, and what's its address?"
   → answered by NRF itself, via Nnrf_NFDiscovery_Search
```

AMF's C++ code still calls `nrfclient::discover("UDM")` exactly as in Part
2 — it has no idea it's running in Kubernetes. The *only* thing Kubernetes
changes is what `NRF_HOST` resolves to (a Service DNS name instead of
`127.0.0.1` or a docker-compose service name). The Go layer's
`go-services-configmap.yaml` does the same thing for its own NRF polling:
`NRF_ADDR: "http://g5-nrf-svc:29510"`.

**Both layers exist simultaneously and solve different problems**: K8s
Service DNS solves "pods are ephemeral, give me a stable network address";
NRF discovery solves "which *logical* NF, by 3GPP role, should I talk to,
and is it currently registered/healthy." A production 5G core needs both —
this is a genuinely good thing to be able to explain clearly if asked "how
does service discovery work in your project," because most candidates
conflate the two.

---

## Part 6: One concrete request, start to finish

Someone runs a scenario and watches KPIs update, all pieces included:

```
1.  Operator: POST /login (auth-service)                → JWT (role=operator)
2.  Operator: POST /scenarios/start, Bearer <JWT>        → control-api verifies JWT,
                                                             exec.Command("./nrf_sim"), ("./udm_sim"), ...
3.  C++ layer boots: udm_sim registers with nrf_sim       → nrf_client.h registerSelf()
    amf_sim starts, discovers udm_sim via nrf_sim         → nrf_client.h discover()
    amf_sim ══socket══▶ udm_sim  (direct connection, NRF no longer involved)
4.  Each C++ NF writes lines to /tmp/g5_<node>.log
5.  event-gateway's logWatcher goroutine (polls every 500ms)
    reads new lines → builds a SimEvent → pub.Publish(event)
6.  Publish() fans out to TWO places at once:
      a) BoundedStore  → any client on GET /events/stream sees it live (SSE), <10ms
      b) KafkaPublisher → topic 5g-sim-events, partitioned by SUPI, async
7.  kpi-consumer (separate pod/process, consumer group "kpi-readers")
    reads the Kafka message, buckets it into a 60s tumbling window
    keyed by (NF, window-start), dedupes by event ID (idempotency)
8.  Every ~60s, that window closes:
    stdout: "[10:01:00] AMF health: 92% ok (23 events)"
    /metrics: kpi_consumer_messages_processed_total++ (Prometheus)
```

Notice steps 3 (C++ discovery) and 5-8 (Go/Kafka observation) are on
**entirely separate timelines** — step 3 already fully worked, with zero
Go/Kafka involvement, before step 4 even starts. That's the "beside, not
inside" relationship from Part 0, made concrete.

---

## Part 7: What has to actually be running, and in what order

For the full picture (not required just to run the C++ sim by itself):

```bash
# 1. Kafka (only needed if you want the event-gateway → kpi-consumer path)
cd 5g-simulator/go-services
docker compose -f docker/docker-compose.kafka.yml up -d kafka

# 2. The C++ NF layer (works standalone, with or without anything below)
#    e.g. via docker compose at 5g-simulator/docker-compose.yml,
#    or via control-api's POST /scenarios/start once auth+control-api are up

# 3. The Go layer (order among these three doesn't matter to each other)
go run ./cmd/auth-service
go run ./cmd/control-api
KAFKA_BROKERS=localhost:9092 go run ./cmd/event-gateway   # omit KAFKA_BROKERS to disable Kafka fan-out

# 4. The Kafka consumer, only meaningful once (1) and an event source (2+3) exist
KAFKA_BROKERS=localhost:9092 go run ./cmd/kpi-consumer
```

Nothing in step 3 or 4 blocks or is required for step 2 to work — that's
the whole architectural point of this document. If you only ever run step
2, you have a fully functional, self-contained 5G core simulator with
real NF discovery, exactly as it was before Go entered this repo at all.
