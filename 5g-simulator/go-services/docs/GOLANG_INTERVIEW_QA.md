# Go Interview Q&A — Explained From Our Code

> **How to use this doc:**
> Every answer tells you first *where exactly in our project this is happening*, then
> explains it simply, then compares it to C++ (since you know that world), then explains
> what it means at production scale. Read it like someone just handed you this project
> on your first week and you need to explain it in a interview the next morning.

---

## Go Core Concepts

---

### Q1. What are goroutines and how does the scheduler work?

**Where in our code:**
File: `go-services/cmd/event-gateway/main.go`, lines 171–172

```go
go func() { defer wg.Done(); logWatcher(ctx, cfg, store) }()
go func() { defer wg.Done(); nrfPoller(ctx, cfg, store) }()
```

These two lines launch two goroutines when event-gateway starts. One polls log files
every 500ms. The other checks the C++ NRF every 5 seconds. Both run forever until
you press Ctrl+C.

---

**What is a goroutine, simply:**

Think of it like starting a thread in C++ — but much lighter.

In C++ if you want two things to run concurrently you do:
```cpp
std::thread t1(logWatcher);
std::thread t2(nrfPoller);
```
Each OS thread costs ~1–8 MB of stack memory. Creating 10,000 threads on a server
would crash it.

A Go goroutine starts with only ~2–8 KB of stack. You can have 100,000 goroutines on
the same machine that would struggle with 1,000 C++ threads. The Go runtime manages
them, not the OS.

---

**The G-M-P model (what the scheduler actually does):**

- **G = Goroutine** — the task (your function + its local variables)
- **M = Machine** — an OS thread (the actual CPU runner)
- **P = Processor** — the "work slot" that connects G to M

Go creates as many Ps as you have CPU cores (`GOMAXPROCS`). Each P has a queue of
goroutines waiting to run. When a goroutine blocks (waiting for a file read, a
network reply, a channel), its M hands the P to another goroutine — so your CPU
stays busy. This is why one Go server can handle 50,000 HTTP connections on 4 cores.

---

**In our sim (small scale):**
We have exactly 3 goroutines running at all times inside event-gateway:
1. `logWatcher` — tailing log files
2. `nrfPoller` — checking NRF
3. The main goroutine — handling HTTP requests

When an SSE client connects (`GET /events/stream`), a 4th goroutine runs for that
client. When it disconnects, it exits.

**In production (big scale):**
A real 5G AMF handles thousands of UE registrations simultaneously. Each registration
is one goroutine. Go's scheduler moves them across CPU cores automatically. You do
not manage any of this — you just write `go handleRegistration(ue)`.

---

**What to say in an interview:**
> "In our event-gateway, we start two long-running goroutines at startup — a log
> watcher and an NRF poller. They're both cancelled via a context when the service
> shuts down. Goroutines are much lighter than OS threads — the Go scheduler (G-M-P
> model) multiplexes millions of them onto a handful of OS threads using work-stealing."

---

### Q2. Channels — unbuffered vs buffered, and the core philosophy

**Where in our code:**

Two places show channels clearly:

**Place 1 — `internal/events/store.go`, line 95:**
```go
func (s *BoundedStore) Subscribe() (int, <-chan SimEvent) {
    ch := make(chan SimEvent, 64)   // buffered channel, 64 events of space
    ...
}
```
Every SSE client that connects gets its own channel with room for 64 events.

**Place 2 — `internal/events/store.go`, lines 53–60:**
```go
select {
case ch <- e:         // try to send the new event to this SSE client
default:              // if channel is full (client too slow) — skip, don't block
}
```

---

**What is a channel, simply:**

A channel is a pipe between two goroutines. One goroutine puts data in; another
takes it out.

In C++ you'd do this with a `std::queue` + `std::mutex` + `std::condition_variable`:
```cpp
std::queue<SimEvent> q;
std::mutex mtx;
std::condition_variable cv;
// producer:  lock → push → notify
// consumer:  lock → wait → pop
```
A Go channel does all of that in one line. The locking and signaling is hidden inside.

**Unbuffered vs buffered:**
- `make(chan SimEvent)` — unbuffered: sender blocks until receiver reads. Like a
  direct handoff. Good for synchronization.
- `make(chan SimEvent, 64)` — buffered: sender can put 64 events in before blocking.
  Good for decoupling producer and consumer speed.

We use buffered (64) for SSE clients because the log-watcher goroutine should not
wait for a slow browser to receive events.

---

**The `select` statement:**

`select` is like a switch-case but for channels. It runs whichever case is ready:

```go
// SSE handler in event-gateway/main.go:
select {
case <-r.Context().Done():   // client disconnected? exit
    return
case ev, ok := <-ch:         // new event available? send it
    fmt.Fprintf(w, "data: %s\n\n", data)
    flusher.Flush()
}
```
This goroutine blocks here, sleeping, using zero CPU — until either the client
disconnects OR a new event arrives. There is no polling loop.

---

**In our sim (small scale):**
One buffered channel (64 slots) per SSE subscriber. The `BoundedStore.Publish()`
method sends to all channels simultaneously using a non-blocking select — slow clients
miss events but never slow down the log-watcher.

**In production (big scale):**
This same pattern scales to thousands of SSE clients. Each gets its own channel.
Kafka replaces the BoundedStore, but the channel pattern for the final delivery to
each client stays the same.

---

### Q3. context.Context — what is it and why does every HTTP handler need it?

**Where in our code:**

**Place 1 — `event-gateway/main.go`, lines 167–171:**
```go
ctx, cancel := context.WithCancel(context.Background())
// ...
go func() { defer wg.Done(); logWatcher(ctx, cfg, store) }()
go func() { defer wg.Done(); nrfPoller(ctx, cfg, store) }()
```
The same `ctx` is passed to both goroutines. When main() calls `cancel()` on
shutdown, both goroutines exit.

**Place 2 — `event-gateway/main.go`, lines 150–152:**
```go
case <-r.Context().Done():
    return   // browser closed the SSE tab — exit this goroutine
```
`r.Context()` is the HTTP request's context. When the browser closes the connection,
Go cancels this context automatically.

---

**What is context.Context, simply:**

In C++ if you want to cancel a background thread, you typically use a shared
atomic bool:
```cpp
std::atomic<bool> stop_flag = false;
// in thread:  while (!stop_flag) { ... }
// from main:  stop_flag = true;
```
Go's `context.Context` is a more structured version of that — it's a tree of
cancellation signals. If you cancel the parent, all children are cancelled
automatically. It also carries a deadline ("cancel after 3 seconds") and can carry
values (like auth claims) without changing function signatures.

**The three variants:**
- `context.WithCancel(parent)` — you cancel it manually (our shutdown pattern)
- `context.WithTimeout(parent, 3*time.Second)` — auto-cancels after 3 seconds
- `context.WithDeadline(parent, t)` — auto-cancels at a specific clock time

---

**What you'd see in logs:**

When you press Ctrl+C on event-gateway:
```
2026/07/08 11:20:05 INFO shutting down event-gateway
2026/07/08 11:20:05 INFO logWatcher exiting           ← ctx.Done() fired
2026/07/08 11:20:05 INFO nrfPoller exiting            ← ctx.Done() fired
2026/07/08 11:20:05 INFO event-gateway stopped
```

If we did NOT use context, both goroutines would keep running after main() exits —
goroutine leak.

---

**In our sim (small scale):**
One context for the whole service. Cancel it on SIGTERM, everything stops cleanly.

**In production (big scale):**
Every outbound HTTP call to NRF, UDM, AUSF gets its own child context with a timeout:
```go
ctx, cancel := context.WithTimeout(r.Context(), 3*time.Second)
defer cancel()
resp, err := http.NewRequestWithContext(ctx, "GET", nrfURL, nil)
```
If the NRF takes more than 3 seconds, the call fails fast instead of waiting forever.

---

### Q4. defer — execution order and common pitfalls

**Where in our code:**

The most important `defer` in the whole project — `event-gateway/main.go`, line 148:
```go
subID, ch := store.Subscribe()
defer store.Unsubscribe(subID)   // ← THIS LINE IS CRITICAL
```

This is inside the SSE stream handler. When the browser closes the tab, the function
returns. The `defer` runs automatically — it removes this client's channel from the
subscriber map and closes the channel.

---

**What is defer, simply:**

In C++ you'd do cleanup in the destructor (RAII) or at the end of a function:
```cpp
void handleSSE() {
    int subID = store.Subscribe();
    // ... handle stream ...
    store.Unsubscribe(subID);  // you must remember this every time
}
```
The problem: if you `return` early (client disconnected, error, panic), you might
skip the cleanup.

`defer` in Go schedules the cleanup at the top of the function and guarantees it runs
no matter how the function exits — normal return, early return, or panic.

**LIFO order — last deferred runs first:**
```go
defer fmt.Println("first deferred")   // prints THIRD
defer fmt.Println("second deferred")  // prints SECOND
defer fmt.Println("third deferred")   // prints FIRST
```
This mirrors C++ destructor order (last constructed, first destroyed).

**The common pitfall — defer inside a loop:**
```go
// BUG: all 100 files stay open until the function returns
for _, path := range paths {
    f, _ := os.Open(path)
    defer f.Close()   // deferred at function scope, NOT at loop iteration
}
```
In our `tailFile()` function we avoid this by using `defer f.Close()` inside a
separate function that's called per-file — so the defer fires at the end of each
`tailFile()` call, not at the end of `logWatcher()`.

---

**Why it matters for goroutine leaks:**

Without `defer store.Unsubscribe(subID)`, every browser that opens the SSE stream
and then closes the tab would leave a dead channel in the subscriber map. The
`Publish()` method would try to send to that channel, block forever (or drop), and
over hours the map would grow. The goroutine for that client would never exit. This
is called a goroutine leak.

---

### Q5. Interfaces — implicit implementation vs C++ explicit virtual

**Where in our code:**

`internal/events/model.go`, lines (at the end of the file):
```go
type Publisher interface {
    Publish(e SimEvent)
}
```

`internal/events/store.go`, line 43:
```go
func (s *BoundedStore) Publish(e SimEvent) {
    // stores event + fans out to all SSE channels
}
```

`BoundedStore` never says "I implement Publisher". It just has the method. Go checks
this at compile time automatically.

---

**Why this matters — compare to C++:**

In C++ you must declare the relationship explicitly:
```cpp
class Publisher {
public:
    virtual void Publish(SimEvent e) = 0;
};

class BoundedStore : public Publisher {  // ← must declare this
public:
    void Publish(SimEvent e) override { ... }
};
```

In Go there is no `: public Publisher`. If `BoundedStore` has a `Publish(SimEvent)`
method, it automatically satisfies the `Publisher` interface. This is called
**structural typing** or **duck typing with compile-time checking**.

**The real benefit — swapping implementations:**

In `event-gateway/main.go`:
```go
store := events.NewBoundedStore(1000)   // implements Publisher

// Pass it as Publisher to both goroutines:
go logWatcher(ctx, cfg, store)   // logWatcher accepts events.Publisher
go nrfPoller(ctx, cfg, store)    // so does nrfPoller
```

To add Kafka: create a `KafkaPublisher` struct with a `Publish(e SimEvent)` method.
Change one line in `main.go`. `logWatcher` and `nrfPoller` need zero changes —
they only know about `Publisher`, not about the store.

---

**In our sim (small scale):**
`Publisher` → `BoundedStore` → fans out to in-memory channels → SSE clients read them.

**In production (big scale):**
`Publisher` → `KafkaPublisher` → Kafka topic → multiple consumer groups read events
→ SSE clients, analytics pipelines, alert systems, all consume the same topic
independently. One interface change, everything else stays the same.

---

### Q6. Error handling — explicit errors vs exceptions

**Where in our code:**

`control-api/main.go`, lines 60–63:
```go
nrfCmd, err := launch("nrf_sim")
if err != nil {
    s.mu.Lock(); s.status = "idle"; s.mu.Unlock()
    return fmt.Errorf("starting nrf_sim: %w", err)
}
```

`event-gateway/main.go` — `fetchNFIDs()`:
```go
resp, err := c.Get(nrfAddr + "/nf-instances")
if err != nil { return nil }   // NRF down = expected, not a crash
```

---

**What is Go error handling, simply:**

Go has no exceptions. No try/catch. No `throw`. Instead, every function that can
fail returns two things: the result and an error:
```go
result, err := someFunction()
if err != nil {
    // handle it
}
```

In C++ you'd write:
```cpp
try {
    auto result = someFunction();
} catch (const std::exception& e) {
    // handle
}
```

Go forces you to handle errors at the call site. You cannot ignore them by accident
(you'll get a "declared and not used" error). This makes the code more predictable.

**Three levels of error detail:**

1. **Simple error:** `errors.New("NRF not reachable")` — just a message
2. **Wrapped error with context:** `fmt.Errorf("starting nrf_sim: %w", err)` — adds
   where it happened, but the original error is still accessible via `errors.Is()`
3. **Custom error type:** for when the caller needs to inspect the error and branch

**In our sim — design choice:**
When the NRF is unreachable, `fetchNFIDs()` returns `nil` (empty map), not an error.
The `/scenarios` endpoint then returns `"nrf_reachable": false` in the JSON. We chose
this because NRF being temporarily down is a normal operational state, not a bug.
The client (a dashboard, a developer) can see the flag and know why.

---

### Q7. sync package — Mutex, RWMutex, WaitGroup, Once

**Where in our code:**

`control-api/main.go` — `ScenarioState`:
```go
type ScenarioState struct {
    mu        sync.Mutex      // ← protects everything below
    status    string
    procs     []*exec.Cmd
    startedAt time.Time
}
```

`internal/events/store.go`:
```go
type BoundedStore struct {
    mu      sync.RWMutex    // ← multiple readers OK; one writer at a time
    muSubs  sync.Mutex      // ← separate lock for subscriber map
    ...
}
```

`event-gateway/main.go`, lines 169–171:
```go
var wg sync.WaitGroup
wg.Add(2)
go func() { defer wg.Done(); logWatcher(ctx, cfg, store) }()
go func() { defer wg.Done(); nrfPoller(ctx, cfg, store) }()
// ... at shutdown:
wg.Wait()   // block until both goroutines exit
```

---

**What each one does, simply:**

**sync.Mutex** — the basic lock. Same as `std::mutex` in C++.
- One goroutine holds it at a time. Others wait.
- Use when any goroutine reads OR writes the same data.

**sync.RWMutex** — like `std::shared_mutex` in C++17.
- Multiple goroutines can read at the same time (RLock).
- Only one goroutine can write (Lock), and it waits for all readers to finish.
- We use this in `BoundedStore` because there can be many SSE clients all calling
  `Recent()` to read events simultaneously, while `Publish()` writes rarely.

**sync.WaitGroup** — a counter. Like `pthread_join` but for multiple goroutines.
- `wg.Add(2)` — "I'm launching 2 goroutines"
- `wg.Done()` — "I (a goroutine) am finished"
- `wg.Wait()` — "block here until Done() has been called 2 times"

**When to use channels instead of mutex:**
If goroutines are *passing data* to each other (like events flowing from log-watcher
to SSE handlers) — use channels.
If goroutines are *sharing state* (like the scenario status field in `ScenarioState`)
— use a mutex. Shared state with a mutex is easier to reason about than state passed
through channels.

---

**Why two mutexes in BoundedStore:**

`mu` protects the circular buffer (the event storage).
`muSubs` protects the subscriber map (the SSE client list).

If we used one mutex for both, `Publish()` would have to hold the lock the entire
time it's fanning out to subscribers — blocking `Recent()` (which readers call).
Separating them means reading recent events never blocks event publishing.

---

### Q8. Race conditions and go test -race

**Where in our code:**

`control-api/main.go`, lines 107–113:
```go
var requestCounters sync.Map   // ← thread-safe map, no lock needed

func incCounter(path string) {
    v, _ := requestCounters.LoadOrStore(path, new(atomic.Int64))
    v.(*atomic.Int64).Add(1)   // ← atomic increment, safe from any goroutine
}
```

We could have used `map[string]int64` + `sync.Mutex` — but `sync.Map` + `atomic.Int64`
avoids a lock entirely for a common operation (incrementing a counter).

---

**What is a race condition, simply:**

Two goroutines read and write the same variable simultaneously. The result is
undefined — whatever goroutine "wins" the race determines the output.

In C++ this is undefined behavior — no warning, no crash (usually), just wrong data.

In Go you can detect races at runtime:
```
go run -race ./cmd/event-gateway/
```
The race detector instruments every memory access. If two goroutines touch the
same variable without synchronization, you get:
```
WARNING: DATA RACE
Write at 0x... by goroutine 7:
  main.incCounter(...)
Read at 0x... by goroutine 12:
  main.metricsText(...)
```
This is invaluable because race conditions in production cause silent data corruption
that's nearly impossible to reproduce.

**The most common race in web servers:**

A handler goroutine reads a shared variable while another handler goroutine writes it.
With a mutex, one waits for the other. Without, you have corruption.

---

**What to say:**
> "Go's race detector is a compile-time + runtime instrumentation tool. We run
> `go test -race ./...` in every CI pipeline. In our BoundedStore, we separated
> two mutexes — one for the buffer, one for subscribers — specifically to avoid
> a lock that would be detected as a bottleneck under load."

---

### Q9. The G-M-P scheduler in depth

*(This extends Q1 — interviewers sometimes ask this as a follow-up.)*

**Where in our code:**
Every `go func()` in the project creates a G. The machine (your laptop) has M OS
threads. The Go runtime creates as many Ps as CPU cores.

---

**The three entities again, with more depth:**

**G (Goroutine):** Holds the function, local variables, and a stack pointer.
Starts at 2–8 KB stack, grows dynamically up to 1 GB if needed.
Goroutines can be in states: runnable, running, blocked, dead.

**M (Machine = OS thread):** Actually runs on a CPU. Go creates Ms on demand.
When all Ms are blocked on syscalls, Go creates new Ms so Ps aren't starved.

**P (Processor):** The "permission slip" for an M to run Go code. Count = GOMAXPROCS.
Each P has a local run queue of G's (up to 256). When a P's queue is empty, it
steals half the queue from a random other P. This is work-stealing.

**Key moments:**

1. `logWatcher` is sleeping on `time.NewTicker(500ms)`. It is not on any P's run
   queue — it's parked. The P is free to run other goroutines. When 500ms passes,
   the runtime puts it back in the queue.

2. `fetchNFIDs` does `http.Get(nrfAddr)`. This is a network I/O. The goroutine
   blocks. Go detects this is a network syscall (via `netpoller` / `epoll`), parks
   the goroutine, and hands the P to another goroutine. When NRF replies, the
   goroutine wakes and re-enters the run queue.

3. `nrfPoller` and `logWatcher` can run in parallel on two different Ps if the
   machine has ≥ 2 cores.

---

**C++ analogy:**

In C++ with `std::thread`, each thread maps 1:1 to an OS thread. You control
scheduling only by creating or blocking threads. Go's scheduler is like a user-space
thread library built into the runtime — goroutines are "green threads" but with
work-stealing and kernel-bypass for I/O.

---

### Q10. Slices, maps, and pointers

**Where in our code:**

`internal/events/store.go`, line 31:
```go
buf: make([]SimEvent, capacity),
```
This creates a slice (a circular buffer) pre-allocated with `capacity` SimEvent slots.
No allocation happens per event — we write into existing slots by index.

`internal/events/store.go`, lines 82–88:
```go
result := make([]SimEvent, n)
startIdx := (s.writeIdx - n + s.capacity*2) % s.capacity
for i := 0; i < n; i++ {
    result[i] = s.buf[(startIdx+i)%s.capacity]
}
return result
```
`Recent()` copies events into a new slice and returns it. The caller gets a snapshot
that can't be affected by future writes to the buffer.

---

**Slices — simply:**

A slice is a three-field struct: `{data *T, len int, cap int}`. Like a `std::vector`
but without ownership — it's a view into an array.

```go
s := make([]int, 3, 6)   // len=3, cap=6, backing array has 6 slots
s2 := s                   // s2 shares the same backing array
s2[0] = 99               // s[0] is now 99 too
s3 := append(s, 1,2,3,4) // cap exceeded → new array allocated → s unchanged
```

In C++:
```cpp
std::vector<int> v = {0, 0, 0};
int* ptr = v.data();         // raw pointer to same data
ptr[0] = 99;                 // v[0] is 99 too
v.push_back(1);              // may reallocate — ptr now dangling
```
Go slices behave similarly but without pointer arithmetic and without dangling pointers
(the GC tracks everything).

**Maps — simply:**

`make(map[string]bool)` is like `std::unordered_map<string, bool>`. The zero value
of a map is `nil` — writing to `nil` map panics. Always `make` before use.

Maps are NOT safe for concurrent access without a lock. In our code, `BoundedStore`
uses `muSubs sync.Mutex` to protect `subs map[int]chan SimEvent`. The request counters
in control-api use `sync.Map` (a built-in concurrent map).

**Pointers — simply:**

`&x` gives you a `*T`. No pointer arithmetic. No manual delete. The garbage collector
frees memory when nothing points to it anymore. Go pointers are safer than C++
pointers but slightly slower (GC overhead) — irrelevant for network-I/O-bound services.

---

## Architecture & Design Questions

---

### Q11. How would you design a Go service that controls a C++ telecom engine?

**Where in our code:**
This is exactly what `control-api` does. File: `control-api/main.go`.

The design is in three layers:

**Layer 1 — auth gate** (lines 165–166):
```go
mux.Handle("/scenarios/start",
    auth.RequireRole(cfg.JWTSecret, auth.RoleOperator, http.HandlerFunc(...)))
```
Only users with role "operator" or "admin" can start/stop the C++ simulator.
Viewers can only read status.

**Layer 2 — bridge to NRF** (lines 127–134):
```go
func fetchNRF(nrfAddr string) (json.RawMessage, bool) {
    c := &http.Client{Timeout: 3 * time.Second}
    resp, err := c.Get(nrfAddr + "/nf-instances")
    ...
}
```
The C++ NRF already has an HTTP server on port 29510. Go doesn't need to "talk C++"
— it just calls the REST API that's already there.

**Layer 3 — process lifecycle** (lines 45–81):
```go
cmd := exec.Command(filepath.Join(cfg.SimBinDir, "nrf_sim"))
cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
return cmd, cmd.Start()
```
`exec.Command` in Go is like `fork/exec` in C++. It starts the C++ binary as a
child process. We store the `*exec.Cmd` pointer so we can send SIGTERM to it later.

---

**In our sim (small scale):**
Go starts the C++ binaries (nrf_sim, amf_sim, etc.) as child processes on the same
machine. They communicate over TCP/HTTP. The Go layer is just the control plane —
it tells C++ what to do and reads NRF's registry to show status.

**In production (big scale):**
The `exec.Command` calls would be replaced with Kubernetes API calls:
```go
// Scale the NRF deployment to 1 replica:
clientset.AppsV1().Deployments(ns).Patch(ctx, "g5-nrf", types.MergePatchType, patch)
```
The Go service stays stateless (no process handles to store). Kubernetes manages
the C++ pod lifecycle. The Go layer only calls the K8s API and reads NRF's HTTP
endpoint — same HTTP bridge, different lifecycle manager.

---

**What to say in an interview:**
> "Our control-api has three jobs: auth (JWT middleware), status (proxy to the C++ NRF's
> HTTP API), and lifecycle (exec.Command in dev mode, would be client-go K8s patches in
> production). The key insight is that the C++ simulator already exposes an HTTP endpoint
> at port 29510 — Go just calls it like any REST API. We don't need shared memory or
> IPC. The boundary is HTTP."

---

### Q12. How would you implement graceful shutdown in Go?

**Where in our code:**
All three services follow the same pattern. Best example: `event-gateway/main.go`,
lines 199–215.

```go
// Step 1: listen for OS signals
quit := make(chan os.Signal, 1)
signal.Notify(quit, os.Interrupt, syscall.SIGTERM)

// Step 2: start the HTTP server in background
go func() { srv.ListenAndServe() }()

// Step 3: block here until signal arrives
<-quit

// Step 4: cancel background goroutines (logWatcher, nrfPoller exit)
cancel()

// Step 5: stop accepting new HTTP connections; finish existing ones (up to 10s)
shutCtx, shutCancel := context.WithTimeout(context.Background(), 10*time.Second)
defer shutCancel()
srv.Shutdown(shutCtx)

// Step 6: wait for background goroutines to fully exit
wg.Wait()
```

---

**Why this matters, simply:**

In C++ you'd handle SIGTERM with a signal handler:
```cpp
signal(SIGTERM, [](int) { running = false; });
while (running) { /* serve */ }
cleanup();
```
The problem: your C++ signal handler can only do async-signal-safe operations.
Go's approach is cleaner — put the signal on a channel, handle it in normal code.

**What "graceful" means:**
- Not graceful: process dies immediately. In-flight requests get a TCP RST. Clients
  see connection errors.
- Graceful: stop accepting NEW connections. Let current requests finish (up to 10s).
  Then exit.

**In K8s:**
When you run `kubectl delete pod g5-event-gateway-xxx`, K8s sends SIGTERM. Our
`<-quit` unblocks. We drain requests for 10 seconds. Then K8s sends SIGKILL. If
our 10-second window is less than K8s's `terminationGracePeriodSeconds` (default 30s),
everything exits cleanly.

**What you'd see in logs:**
```
2026/07/08 11:30:00 INFO event-gateway starting addr=:8083
...
[user runs: kubectl delete pod g5-event-gateway OR Ctrl+C]
2026/07/08 11:35:00 INFO shutting down event-gateway
2026/07/08 11:35:00 INFO event-gateway stopped
```

---

### Q13. How would you prevent goroutine leaks?

**Where in our code:**
`event-gateway/main.go`, lines 147–161 — the SSE handler:

```go
subID, ch := store.Subscribe()
defer store.Unsubscribe(subID)   // THE CRITICAL LINE

for {
    select {
    case <-r.Context().Done():
        return   // client closed tab → deferred Unsubscribe runs
    case ev, ok := <-ch:
        if !ok { return }
        fmt.Fprintf(w, "data: %s\n\n", data)
        flusher.Flush()
    }
}
```

---

**What is a goroutine leak, simply:**

A goroutine that never exits. It's like a thread that's stuck waiting and nobody
ever kills it. Over time, leaked goroutines accumulate — each one holds memory,
a stack, and potentially open file descriptors.

The SSE endpoint is the classic place to leak:
1. Browser opens `GET /events/stream`
2. A goroutine starts — blocks in the `select` above waiting for events
3. Browser closes the tab
4. Without `defer Unsubscribe`: the goroutine's channel stays in the subscriber map.
   `Publish()` tries to send to it. The goroutine never reads the channel (browser is
   gone). The channel buffer fills up. Now every `Publish()` call is doing a no-op
   select-default for that dead channel — forever.
5. After 1000 users open and close the SSE tab: 1000 leaked goroutines.

**With `defer Unsubscribe`:**
When the browser closes the tab, `r.Context().Done()` fires. The `return` at line 152
runs. `defer` kicks in. `store.Unsubscribe(subID)` removes the channel from the map
and closes it. The goroutine is gone. Clean.

**How to detect leaks in tests:**
```go
import "go.uber.org/goleak"

func TestSSEStream(t *testing.T) {
    defer goleak.VerifyNone(t)   // fails if any goroutines remain after test
    ...
}
```

**In C++:** goroutine leaks are equivalent to thread leaks — threads that are
blocked waiting on a condition_variable that will never fire. The fix is the same:
ensure that when the "client" disconnects, you signal the waiting thread to exit.

---

### Q14. How would you add observability to this Go layer?

**Where in our code:**
`control-api/main.go`, lines 107–125 — manual Prometheus metrics:

```go
var requestCounters sync.Map

func incCounter(path string) {
    v, _ := requestCounters.LoadOrStore(path, new(atomic.Int64))
    v.(*atomic.Int64).Add(1)
}

func metricsText(running bool) string {
    // outputs text like:
    // # HELP go_scenario_status Current scenario status
    // go_scenario_status 1
    // go_requests_total{path="/scenarios"} 42
}
```

---

**Three pillars of observability:**

**1. Metrics (counters, gauges, histograms):**
What we built: manual Prometheus text format at `/metrics`.
What production uses: `prometheus/client_golang` library with actual histograms
for latency (p50, p99), counters for errors, gauges for active connections.

```go
// Production upgrade: replace our manual incCounter with:
var httpLatency = prometheus.NewHistogramVec(
    prometheus.HistogramOpts{Name: "http_duration_seconds"},
    []string{"method", "path", "status"},
)
// Grafana reads /metrics every 15s and draws graphs.
```

**2. Logs (structured JSON):**
Already done with `log/slog`. In production, output JSON:
```go
slog.New(slog.NewJSONHandler(os.Stdout, nil))
// Output: {"time":"2026-07-08T11:30:00Z","level":"INFO","msg":"request","path":"/scenarios","latency_ms":2}
// Loki/Elasticsearch picks this up automatically.
```

**3. Traces (request ID through the whole system):**
Add `traceparent` header to every outbound HTTP call. OpenTelemetry propagates it
through NRF, AMF, UDM — so in Jaeger you can see one UE registration as a single
trace spanning all 5 NFs.

**In our sim:** metrics are manual text. Logs go to stdout.
**In production:** Prometheus scrapes `/metrics` every 15s. Grafana shows dashboards.
PagerDuty alerts fire when `http_errors_total` exceeds a threshold.

---

### Q15. How would you handle retries and timeouts between services?

**Where in our code:**
`event-gateway/main.go`, lines 121–122:
```go
c := &http.Client{Timeout: 3 * time.Second}
resp, err := c.Get(nrfAddr + "/nf-instances")
```

`control-api/main.go`, lines 128–129:
```go
c := &http.Client{Timeout: 3 * time.Second}
resp, err := c.Get(nrfAddr + "/nf-instances")
```

Both services use a 3-second timeout for the NRF call. No retry — if NRF is down,
we return `nrf_reachable: false` and move on.

---

**Why timeout matters, simply:**

Go's default `http.Client` has no timeout. If you call `http.Get(url)` with
`http.DefaultClient` and the server never responds, the goroutine waits forever.
One slow downstream service can cause your goroutines to pile up until the process
runs out of memory.

Always create your own client:
```go
client := &http.Client{Timeout: 3 * time.Second}
```

In C++ with curl you'd set `CURLOPT_TIMEOUT`. Same idea.

**When to add retries:**

If the downstream service is temporarily unavailable (503) and expected to recover:
```go
for attempt := 0; attempt < 3; attempt++ {
    resp, err := client.Get(url)
    if err == nil && resp.StatusCode < 500 {
        return resp, nil
    }
    // exponential backoff: 100ms, 200ms, 400ms
    time.Sleep(time.Duration(1<<attempt) * 100 * time.Millisecond)
}
```

**When NOT to retry:**
- 401 Unauthorized — retry won't fix a bad token
- 400 Bad Request — retry won't fix bad input
- Only retry on 503/502/timeout (server temporarily overloaded)

**In production — circuit breaker:**
After 5 consecutive failures, stop retrying for 30 seconds (open circuit). Let the
downstream service recover without being hammered by retries. Close the circuit and
retry again after 30 seconds. Library: `sony/gobreaker`.

---

### Q16. When would you choose Go over C++, and vice versa?

**Where in our code:**
This project IS the answer. The split:

```
C++ side:                      Go side:
──────────────────────────     ──────────────────────────
nrf_sim, amf_sim, smf_sim      auth-service
udm_sim, upf_sim, gnb_sim      control-api
                               event-gateway

Why C++?                       Why Go?
- NGAP/S1AP ASN.1 PER          - HTTP/JSON REST APIs
- SCTP / GTP-U packets         - Goroutines for SSE streaming
- Nanosecond latency (UPF)     - Graceful shutdown in 10 lines
- DPDK poll loop (busy-wait)   - Docker image: ~15 MB binary
- Existing protocol stacks     - Kubernetes ConfigMap/Secret native
```

---

**Simple way to explain the split:**

The C++ side handles things where every microsecond matters — encoding a 40-byte
ASN.1 binary blob, forwarding a GTP-U packet at line rate, running a DPDK poll loop
that burns 100% of one CPU core.

The Go side handles things where you want developer productivity and cloud-native
features — REST APIs for a dashboard, streaming events to a browser, reading Kubernetes
secrets from environment variables, building a Docker image in 30 seconds.

**What the industry does:**
Ericsson and Nokia write their 5G SBI (the HTTP/JSON interface between AMF, SMF, UDM)
in Java or Go. The N2 (NGAP) and N3 (GTP-U) packet paths stay in C or C++. This
project follows the exact same architecture.

---

### Q17. How would you make a Go service Kubernetes-ready?

**Where in our code:**
`go-services/k8s/auth-service-deployment.yaml`:
```yaml
readinessProbe:
  httpGet:
    path: /health
    port: 8081
  initialDelaySeconds: 2
  periodSeconds: 5
livenessProbe:
  httpGet:
    path: /health
    port: 8081
  initialDelaySeconds: 10
  periodSeconds: 10
```

`go-services/k8s/go-services-configmap.yaml`:
```yaml
data:
  AUTH_PORT: "8081"
  NRF_ADDR: "http://g5-nrf-svc:29510"
```

---

**The 5 things K8s needs from your service:**

**1. `/health` endpoint:**
K8s probes this to decide if the pod is ready for traffic (readiness) and if it
should be restarted (liveness). Our `health.Handler()` in `internal/health/handler.go`
returns `{"status":"ok"}`. That's all K8s needs.

**2. Graceful shutdown (SIGTERM):**
When K8s deletes a pod (`kubectl delete pod`), it sends SIGTERM. Your service has
`terminationGracePeriodSeconds` (default 30s) to finish work and exit. If it doesn't
exit, K8s sends SIGKILL. Our `<-quit → srv.Shutdown(10s)` pattern fits comfortably
inside that window.

**3. Env-based config:**
K8s passes config via environment variables (from ConfigMap) and secrets (from Secret).
Our `config.Load()` reads them with `os.Getenv()`. You never hardcode a hostname
in Go code — you read `NRF_ADDR` from the environment.

**4. Stateless design:**
Our auth-service stores no session state (JWT is self-contained). Our event-gateway's
`BoundedStore` lives in memory — if the pod restarts, the last 1000 events are gone.
That's fine for a simulator. In production you'd use Redis.

**5. Resource limits:**
In the deployment YAML:
```yaml
resources:
  requests: { memory: "32Mi", cpu: "50m" }
  limits:   { memory: "128Mi", cpu: "200m" }
```
Go binaries are small. Our auth-service binary is ~8 MB. 128 MB memory limit is
generous. Without limits, one runaway service can starve other pods on the same node.

---

### Q18. How would you introduce Kafka in this architecture?

**Where in our code:**
The `Publisher` interface in `internal/events/model.go` is the only code that changes.

Right now:
```
logWatcher  →  store.Publish()  →  BoundedStore  →  SSE clients
nrfPoller   →  store.Publish()         ↑
                                  (in memory, 1000 events)
```

With Kafka:
```
logWatcher  →  kafka.Publish()  →  Kafka topic  →  Consumer group  →  SSE clients
nrfPoller   →  kafka.Publish()        ↑               ↑
                              "5g-sim-events"    event-gateway reads
```

---

**What changes in code:**

```go
// Step 1: implement KafkaPublisher (using IBM/sarama library)
type KafkaPublisher struct {
    producer sarama.SyncProducer
    topic    string
}
func (k *KafkaPublisher) Publish(e events.SimEvent) {
    data, _ := json.Marshal(e)
    k.producer.SendMessage(&sarama.ProducerMessage{
        Topic: k.topic,
        Key:   sarama.StringEncoder(string(e.Node)),  // partition by NF type
        Value: sarama.ByteEncoder(data),
    })
}

// Step 2: in event-gateway/main.go, swap one line:
// Before: store := events.NewBoundedStore(1000)
// After:  store := &KafkaPublisher{producer: p, topic: "5g-sim-events"}

// logWatcher, nrfPoller — unchanged, they accept events.Publisher
go logWatcher(ctx, cfg, store)
go nrfPoller(ctx, cfg, store)
```

---

**Why Kafka instead of in-memory store:**

Our `BoundedStore` holds 1000 events in memory. If the pod restarts, they're gone.
If you have two `event-gateway` replicas, each has its own copy — they diverge.

Kafka is a durable, distributed event log. Events survive pod restarts. Multiple
event-gateway replicas can be consumer group members — Kafka distributes partitions
among them automatically. You can replay events from the beginning of the topic
for debugging.

**What to say simply:**
> "We designed the Publisher interface specifically so Kafka can be plugged in later.
> Right now `BoundedStore` implements Publisher — it's an in-memory ring buffer.
> To add Kafka, create `KafkaPublisher` with the same `Publish()` method, swap it
> in `main.go`. Zero changes to logWatcher or nrfPoller — they only know about
> the Publisher interface, not the implementation."

---

### Q19. How would you add TLS to this layer?

**Where in our code:**
All three services use `srv.ListenAndServe()`. Changing to TLS is:
```go
// Before:
srv.ListenAndServe()

// After (with a cert and key file):
srv.ListenAndServeTLS("/etc/tls/tls.crt", "/etc/tls/tls.key")
```

That's the entire code change. 5 characters.

---

**Where the cert comes from in K8s:**
1. Deploy `cert-manager` (an open-source K8s operator)
2. Create a `Certificate` resource pointing at your CA or Let's Encrypt
3. cert-manager creates a Kubernetes Secret with `tls.crt` and `tls.key`
4. Mount that Secret as a volume in the pod at `/etc/tls/`
5. The Go binary reads the files at startup

cert-manager auto-rotates the cert before expiry. For hot reload without restart:
use `tls.Config.GetCertificate` — a callback that re-reads the file on every TLS
handshake.

**Mutual TLS (mTLS) — for internal service communication:**
Each service presents a certificate. The server verifies the client's cert. This
means even if someone gets inside your K8s network, they can't call the auth-service
without a valid client cert. This is what Istio/service-mesh does automatically.

**In our sim:** TLS is intentionally skipped — minikube dev setup, localhost only.
Add it when the service needs to be exposed outside the cluster.

---

### Q20. Bounded worker pools — why and how?

**Where in our code:**
`event-gateway/main.go`, the `logWatcher` goroutine (line 55–60):
```go
matches, _ := filepath.Glob(filepath.Join(cfg.SimLogDir, "g5_*.log"))
for _, path := range matches {
    tailFile(path, offsets, pub)   // sequential, no pool
}
```
We tail files sequentially because there are only ~6 log files (one per C++ NF).
Sequential is simpler and fast enough.

---

**When you need a bounded pool:**

Imagine the simulator generates 1000 pcap files and you want to parse all of them
concurrently. Launching 1000 goroutines at once is fine for I/O-bound work, but if
each goroutine does CPU-heavy work (parsing, compression), you'd use all CPU cores
and starve other goroutines.

**The semaphore pattern (buffered channel as a token pool):**
```go
maxWorkers := runtime.NumCPU()
sem := make(chan struct{}, maxWorkers)  // only maxWorkers tokens

for _, file := range files {
    sem <- struct{}{}      // acquire a token (blocks when all workers busy)
    go func(f string) {
        defer func() { <-sem }()  // release token when done
        parsePcap(f)
    }(file)
}

// drain: wait for all workers to finish
for i := 0; i < maxWorkers; i++ { sem <- struct{}{} }
```

**C++ equivalent:**
```cpp
// ThreadPool with fixed worker count, task queue
ThreadPool pool(std::thread::hardware_concurrency());
for (auto& file : files) {
    pool.enqueue([&file]{ parsePcap(file); });
}
pool.wait();
```

The Go semaphore pattern is simpler than a full thread pool because goroutines are
cheap — you still launch one goroutine per file, you just limit how many run at once.

**In our sim:**
No bounded pool currently. If we add batch pcap export (parsing 100 files at once),
we'd use `runtime.NumCPU()` as the pool size.

---

*End of Go Interview Q&A — all 20 questions grounded in actual code from this repo.*
