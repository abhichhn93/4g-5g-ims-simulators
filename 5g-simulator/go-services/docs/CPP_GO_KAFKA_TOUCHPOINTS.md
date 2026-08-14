# C++ ↔ Go ↔ Kafka — Exact Code Touchpoints

Two features traced end-to-end: every file, every line where one system hands off to another.

---

## How the Three Layers Sit

```
┌─────────────────────────────────────────────────────┐
│  C++ Core (src/)                                     │
│  AMF / NRF / UDM / SMF / UPF — 3GPP protocol logic  │
│  Writes: g5_amf_session.log  (text)                  │
│  Exposes: HTTP :29510  (NRF REST)                    │
└────────────────┬────────────────────────────────────┘
                 │ (A) log file on disk   (B) HTTP GET /nf-instances
                 ▼
┌─────────────────────────────────────────────────────┐
│  Go Layer (go-services/)                             │
│  event-gateway — two background goroutines:          │
│    logWatcher  polls g5_*.log every 500ms            │
│    nrfPoller   polls NRF HTTP every 5s               │
│  Publishes SimEvent structs                          │
│    → BoundedStore  (SSE /events/stream, zero-latency)│
│    → KafkaPublisher  (durable, replayable)           │
└────────────────┬────────────────────────────────────┘
                 │ kafka.Message{Key, Value: JSON}
                 ▼
┌─────────────────────────────────────────────────────┐
│  Kafka topic  (e.g. "5g-sim-events")                 │
│  Partitioned by SUPI hash / Node                     │
└────────────────┬────────────────────────────────────┘
                 │ kafka-go reader.ReadMessage()
                 ▼
┌─────────────────────────────────────────────────────┐
│  Go kpi-consumer                                     │
│  Reads Kafka, aggregates per-NF health KPI           │
│  Exposes /metrics (Prometheus)                       │
└─────────────────────────────────────────────────────┘
```

---

## Feature 1 — NRF Service Discovery (C++ registers → Go polls → Kafka → KPI)

### What it does
Every C++ network function (AMF, UDM, SMF) announces itself to the C++ NRF at startup.
Go's `nrfPoller` watches that NRF. When a new NF appears or disappears, Go publishes
an `NF_REGISTERED` / `NF_DEREGISTERED` event to both SSE clients and Kafka.
The `kpi-consumer` on Kafka counts those events in tumbling 1-minute windows and prints
per-NF health percentages.

---

### Step 1 — C++ AMF registers itself with C++ NRF

**File:** `src/amf/amf_main.cpp` **line 351**
```cpp
nrfclient::registerSelf(Logger::CLR_AMF, " AMF  ", "amf-1", "AMF", AMF_SELF_HOST, N2_PORT);
```
This calls into the shared NRF client header.

**File:** `src/common/nrf_client.h` **lines 37–58** — `registerSelf()`
```cpp
// line 46 — builds the HTTP request
std::string req = httpBuild(
    "PUT /nnrf-nfm/v1/nf-instances/" + nfInstanceId + " HTTP/1.1", body);

// line 51 — dials the C++ NRF
Socket nrf = Socket::connectTo(nrfHost().c_str(), NRF_PORT);  // port 29510

// line 56 — sends PUT over raw TCP socket
httpSend(nrf, req);
```
Body is JSON: `{"nfInstanceId":"amf-1", "nfType":"AMF", "host":"...", "port":38412}`

**API used:** `PUT /nnrf-nfm/v1/nf-instances/amf-1` — TS 29.510 NRF Management interface.

---

### Step 2 — C++ NRF handles the PUT and stores the profile

**File:** `src/nrf/nrf_main.cpp`

**line 159** — HTTP dispatcher identifies the route:
```cpp
if (method == "PUT" && path.find("/nnrf-nfm/v1/nf-instances/") != std::string::npos) {
    resp = handleRegister(nfInstanceId, req.body);   // line 162
```

**line 84** — `handleRegister()` stores it in a `std::map`:
```cpp
// line 89
g_profiles[nfType] = NfProfile{nfInstanceId, nfType, host, port};
```
`g_profiles` is a static `std::map<std::string, NfProfile>` (line 81) — the NRF's in-memory registry.
Responds `HTTP/1.1 201 Created`.

---

### Step 3 — Go nrfPoller polls the C++ NRF every 5 seconds

**File:** `go-services/cmd/event-gateway/main.go`

**line 123** — goroutine started from `main()` (line 224):
```go
func nrfPoller(ctx context.Context, cfg config.Config, pub events.Publisher) {
    tick := time.NewTicker(5 * time.Second)   // line 124
    defer tick.Stop()
    known := make(map[string]bool)
    for {
        select {
        case <-ctx.Done(): return
        case <-tick.C:
            current := fetchNFIDs(cfg.NRFAddr)   // line 132 — HTTP GET to C++ NRF
```

**line 148** — `fetchNFIDs()` — the actual HTTP call into C++:
```go
func fetchNFIDs(nrfAddr string) map[string]bool {
    c := &http.Client{Timeout: 3 * time.Second}
    resp, err := c.Get(nrfAddr + "/nf-instances")   // line 150
```
**API used:** `GET /nf-instances` → C++ NRF routes to `handleDiscover()` (nrf_main.cpp line 164).

**line 133–143** — diff against previous poll → publish events:
```go
for id := range current {
    if !known[id] {
        pub.Publish(events.SimEvent{          // NEW NF appeared
            EventType: events.EventNFRegistered,
            Node:      events.NodeNRF,
            Detail:    "nfInstanceId=" + id,
        })
    }
}
for id := range known {
    if !current[id] {
        pub.Publish(events.SimEvent{          // NF disappeared
            EventType: events.EventNFDeregistered,
```

---

### Step 4 — Go FanOutPublisher sends to BoundedStore AND Kafka simultaneously

**File:** `go-services/cmd/event-gateway/main.go` **lines 214–217**
```go
if cfg.KafkaBrokers != "" {
    kp := events.NewKafkaPublisher(cfg.KafkaBrokers, cfg.KafkaTopic)  // line 215
    defer kp.Close()
    pub = &events.FanOutPublisher{Local: store, Kafka: kp}            // line 217
}
```

**File:** `go-services/internal/events/kafka.go` **line 77** — `FanOutPublisher.Publish()`:
```go
func (f *FanOutPublisher) Publish(e SimEvent) {
    f.Local.Publish(e)   // → BoundedStore circular buffer → SSE clients
    f.Kafka.Publish(e)   // → Kafka broker
}
```

**File:** `go-services/internal/events/kafka.go` **line 55** — `KafkaPublisher.Publish()`:
```go
func (k *KafkaPublisher) Publish(e SimEvent) {
    key := e.SUPI         // NF_REGISTERED has no SUPI
    if key == "" {
        key = string(e.Node)   // so key = "NRF" → all NRF events in same partition
    }
    data, _ := json.Marshal(e)
    ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
    defer cancel()
    k.writer.WriteMessages(ctx, kafka.Message{Key: []byte(key), Value: data})
```
**Loop used:** No loop here — `kafka.Writer` batches internally with `BatchTimeout: 100ms` (line 34).
**Kafka key:** `"NRF"` → all NF registration events go to the same partition → ordering guaranteed per NF.

---

### Step 5 — kpi-consumer reads from Kafka and aggregates health

**File:** `go-services/cmd/kpi-consumer/main.go`

**Main consume loop** — lines 177–188:
```go
for {
    m, err := reader.ReadMessage(ctx)   // BLOCKING — wakes on each Kafka message
    if err != nil {
        if ctx.Err() != nil { break }   // clean shutdown
        continue
    }
    consumerLag.Set(float64(reader.Stats().Lag))   // Prometheus gauge
    processWithRetry(dlqWriter, m.Key, m.Value, agg)
}
```
**Loop used:** blocking `for` loop — `reader.ReadMessage()` parks the goroutine until Kafka delivers a message.
**No busy-wait:** goroutine sleeps in the kafka-go network poller, 0% CPU when idle.

**`processWithRetry()`** — lines 115–131:
```go
for attempt := 0; ; attempt++ {
    if err := a.ingest(raw); err != nil {
        // exponential backoff: 1s, 2s, 4s
        time.Sleep(backoffs[attempt])
        continue
    }
    messagesProcessed.Inc()   // Prometheus counter
    return
}
// after 3 retries → DLQ
dlq.WriteMessages(ctx, kafka.Message{Key: key, Value: raw})
```

**`aggregator.ingest()`** — lines 72–100 — tumbling window bucketing:
```go
windowStart := e.Timestamp.Truncate(a.windowSize)   // round down to minute boundary
key := windowKey{node: e.Node, start: windowStart}
// ...
w.total++
if e.Severity == "error" { w.errors++ }
```

**`closeExpiredWindows()`** — line 103 — 5-second ticker prints KPI when window closes:
```go
ticker := time.NewTicker(5 * time.Second)
// ...
for key, w := range a.windows {
    if !w.printed && now.After(key.start.Add(a.windowSize).Add(grace)) {
        w.printed = true
        printKPI(key.node, key.start, w)
        // >> [10:01:00] NRF health: 100% ok (3 events)
    }
}
```

---

## Feature 2 — Log Tailing (C++ writes text logs → Go watches → Kafka → KPI)

### What it does
Every C++ NF writes human-readable logs to a file (`g5_amf_session.log`, etc.).
Go's `logWatcher` goroutine tails those files every 500ms, turning each new line into
a `LOG_LINE` `SimEvent`. That event fans out to SSE clients (instant) and Kafka (durable).
`kpi-consumer` reads those events and counts `severity=error` ones for the health %.

---

### Step 1 — C++ AMF opens its log file

**File:** `src/amf/amf_main.cpp` **line 330**
```cpp
Logger::setSessionFile("g5_amf_session.log");
```

**File:** `src/common/logger.h` **lines 62–77** — `setSessionFile()` + first write:
```cpp
// line 62
inline void setSessionFile(const std::string& name) { sessionFileName() = name; }

// line 64 — lazy open on first write
inline std::ofstream& getLogFile() {
    static std::ofstream file;
    if (!file.is_open()) {
        file.open(sessionFileName(), std::ios::out | std::ios::trunc);   // line 67
    }
    return file;
}
```
Every `Logger::step()`, `Logger::sys()`, `Logger::warn()` call in AMF writes a text line
to `g5_amf_session.log` on disk.

Example line written at **amf_main.cpp line 157**:
```cpp
Logger::step("Registration started: " + suci);
// → writes: "[HH:MM:SS] Registration started: suci-0-404-10-..."
```

---

### Step 2 — Go logWatcher polls the file every 500ms

**File:** `go-services/cmd/event-gateway/main.go`

**line 54** — `logWatcher()`:
```go
func logWatcher(ctx context.Context, cfg config.Config, pub events.Publisher) {
    offsets := make(map[string]int64)        // line 55 — per-file byte position
    tick := time.NewTicker(500 * time.Millisecond)   // line 56
    defer tick.Stop()
    for {
        select {
        case <-ctx.Done(): return
        case <-tick.C:
            matches, _ := filepath.Glob(                             // line 63
                filepath.Join(cfg.SimLogDir, "g5_*.log"))           // matches ALL NF logs
            for _, path := range matches {
                tailFile(path, offsets, pub)                         // line 65
            }
        }
    }
}
```
**Loop used:** `select` inside `for` — wakes every 500ms on `tick.C`, or immediately on `ctx.Done()` for clean shutdown.

---

### Step 3 — tailFile() reads only new bytes since last poll

**File:** `go-services/cmd/event-gateway/main.go` **line 71**
```go
func tailFile(path string, offsets map[string]int64, pub events.Publisher) {
    f, _ := os.Open(path)
    defer f.Close()

    if _, known := offsets[path]; !known {        // line 77 — first time seen
        end, _ := f.Seek(0, io.SeekEnd)           // seek to end — skip old history
        offsets[path] = end
        return
    }
    f.Seek(offsets[path], io.SeekStart)           // line 82 — resume from last position

    sc := bufio.NewScanner(f)                     // line 85
    node := nodeFromPath(path)                    // "g5_amf_..." → NodeAMF
    for sc.Scan() {                               // reads one line per iteration
        if line := sc.Text(); line != "" {
            sev := "info"
            if strings.Contains(strings.ToUpper(line), "ERROR") { sev = "error" }
            if strings.Contains(strings.ToUpper(line), "WARN")  { sev = "warn" }

            pub.Publish(events.SimEvent{          // line ~95
                Node:      node,
                EventType: events.EventLogLine,
                Detail:    line,
                Severity:  sev,
            })
        }
    }
    pos, _ := f.Seek(0, io.SeekCurrent)
    offsets[path] = pos                           // line 100 — save new position
}
```
**Key design:** byte offset map avoids re-reading old log lines. Each 500ms tick picks up only what C++ wrote since last check.

---

### Step 4 — SimEvent fans out to SSE + Kafka (same as Feature 1, Step 4)

`pub.Publish()` calls `FanOutPublisher.Publish()` in `kafka.go` line 77.

For `LOG_LINE` events the Kafka key is set differently:
```go
// kafka.go line 55
key := e.SUPI      // LOG_LINE has no SUPI
if key == "" {
    key = string(e.Node)   // → "AMF", "UDM", etc.
}
```
All AMF log lines go to the same Kafka partition → per-NF log ordering preserved.

---

### Step 5 — kpi-consumer counts error lines for health %

Same consume loop as Feature 1. The difference is in `ingest()` — `LOG_LINE` events
are the bulk of what arrives. The `severity` field set in `tailFile()` (line ~94) is
what determines whether an event counts as an error:

```go
// kpi-consumer/main.go line 97
w.total++
if e.Severity == "error" { w.errors++ }   // "error" was set in tailFile()
```

KPI printed every minute:
```
[10:02:00] AMF health: 94% ok (47 events)
[10:02:00] UDM health: 100% ok (12 events)
```
A logged `WARN`/`ERROR` in C++ (`Logger::warn(...)`) → `"error"` severity in Go → reduces the KPI %.

---

## All Touch Points in One Table

| Step | File | Line | What happens |
|------|------|------|-------------|
| **Feature 1** | | | |
| AMF registers with NRF | `src/amf/amf_main.cpp` | 351 | `nrfclient::registerSelf()` called |
| NRF client builds PUT | `src/common/nrf_client.h` | 46 | `PUT /nnrf-nfm/v1/nf-instances/amf-1` |
| NRF stores profile | `src/nrf/nrf_main.cpp` | 89 | `g_profiles["AMF"] = NfProfile{...}` |
| Go polls NRF | `go-services/cmd/event-gateway/main.go` | 124 | `time.NewTicker(5s)` |
| Go HTTP GET to C++ NRF | `go-services/cmd/event-gateway/main.go` | 150 | `c.Get(nrfAddr + "/nf-instances")` |
| Diff + publish | `go-services/cmd/event-gateway/main.go` | 133 | `pub.Publish(NF_REGISTERED)` |
| Fan-out to Kafka | `go-services/internal/events/kafka.go` | 55 | `k.writer.WriteMessages(...)` |
| Kafka consume loop | `go-services/cmd/kpi-consumer/main.go` | 177 | `reader.ReadMessage(ctx)` |
| Window aggregate | `go-services/cmd/kpi-consumer/main.go` | 97 | `w.total++` / `w.errors++` |
| KPI print | `go-services/cmd/kpi-consumer/main.go` | 103 | `closeExpiredWindows()` ticker |
| **Feature 2** | | | |
| AMF sets log file | `src/amf/amf_main.cpp` | 330 | `Logger::setSessionFile("g5_amf_session.log")` |
| Logger opens file | `src/common/logger.h` | 67 | `file.open(sessionFileName(), trunc)` |
| AMF writes log line | `src/amf/amf_main.cpp` | 157 | `Logger::step("Registration started...")` |
| Go polls log dir | `go-services/cmd/event-gateway/main.go` | 56 | `time.NewTicker(500ms)` |
| Go globs log files | `go-services/cmd/event-gateway/main.go` | 63 | `filepath.Glob("g5_*.log")` |
| tailFile seeks new bytes | `go-services/cmd/event-gateway/main.go` | 82 | `f.Seek(offsets[path], SeekStart)` |
| Scan new lines | `go-services/cmd/event-gateway/main.go` | 85 | `bufio.NewScanner(f)` + `sc.Scan()` |
| Publish LOG_LINE event | `go-services/cmd/event-gateway/main.go` | ~95 | `pub.Publish(SimEvent{LOG_LINE})` |
| Fan-out to Kafka | `go-services/internal/events/kafka.go` | 55 | keyed by Node name |
| Kafka consume + KPI | `go-services/cmd/kpi-consumer/main.go` | 97 | counts errors from log severity |

---

## Loops Used — Summary

| Loop | File | What it waits on |
|------|------|-----------------|
| `nrfPoller` — `select` on ticker | event-gateway/main.go:124 | `time.NewTicker(5s)` — wakes every 5s |
| `logWatcher` — `select` on ticker | event-gateway/main.go:56 | `time.NewTicker(500ms)` — wakes every 500ms |
| `tailFile` inner loop | event-gateway/main.go:85 | `bufio.Scanner.Scan()` — one line per iteration |
| `kpi-consumer` main loop | kpi-consumer/main.go:177 | `reader.ReadMessage(ctx)` — BLOCKING, goroutine sleeps |
| `closeExpiredWindows` | kpi-consumer/main.go:103 | `time.NewTicker(5s)` — checks for closed windows |
| `processWithRetry` | kpi-consumer/main.go:115 | `for attempt := 0;;` + `time.Sleep(backoff)` |
| `sseStream` SSE clients | event-gateway/main.go:~175 | `select` on `ch <-` channel or `r.Context().Done()` |
