// event-gateway: aggregates C++ log lines and NRF changes, streams via SSE.
//
// Endpoints:
//
//	GET /health         — no auth
//	GET /events         — last N events as JSON (role: viewer)
//	GET /events/stream  — live SSE stream (role: viewer)
//
// Background goroutines (cancelled on shutdown via context):
//
//	logWatcher — polls SimLogDir/g5_*.log every 500ms, publishes new lines
//	nrfPoller  — queries NRF every 5s, publishes NF registration changes
//
// OTEL metrics added:
//
//	event_gateway_events_published_total  — counter, labelled by node type
//	event_gateway_subscribers_active      — gauge, current SSE subscribers
//
// These appear in /metrics alongside any promauto counters because the
// Prometheus bridge (telemetry.Init) merges both into DefaultRegisterer.
package main

import (
	"bufio"
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"

	"go-services/internal/auth"
	"go-services/internal/config"
	"go-services/internal/events"
	"go-services/internal/health"
	"go-services/internal/telemetry"
)

var (
	eidMu sync.Mutex
	eidN  int64
)

func newID() string {
	eidMu.Lock()
	eidN++
	id := eidN
	eidMu.Unlock()
	return fmt.Sprintf("ev-%d", id)
}

// gatewayMetrics holds OTEL metric instruments for the event-gateway.
// Created once in main(), passed to goroutines that need to record values.
//
// Why OTEL here instead of promauto?
// event-gateway has no /metrics endpoint of its own yet. Using OTEL metrics
// and the Prometheus bridge, they'll appear on kpi-consumer's /metrics
// automatically — or we can add promhttp.Handler() to event-gateway's mux.
// The key point: OTEL metrics and promauto counters live side-by-side in the
// same Prometheus registry via the bridge.
type gatewayMetrics struct {
	eventsPublished  metric.Int64Counter  // event_gateway_events_published_total{node=...}
	subscriberCount  metric.Int64UpDownCounter // event_gateway_subscribers_active (can go up AND down)
	activeSubscribers atomic.Int64         // tracks current count for SSE handlers
}

func newGatewayMetrics(meter metric.Meter) (*gatewayMetrics, error) {
	eventsPublished, err := meter.Int64Counter(
		"event_gateway.events_published",
		metric.WithDescription("Total SimEvents published through the gateway, by source node"),
		metric.WithUnit("{event}"),
	)
	if err != nil {
		return nil, err
	}

	subscriberCount, err := meter.Int64UpDownCounter(
		"event_gateway.subscribers_active",
		metric.WithDescription("Current number of active SSE subscribers"),
		metric.WithUnit("{subscriber}"),
	)
	if err != nil {
		return nil, err
	}

	return &gatewayMetrics{
		eventsPublished: eventsPublished,
		subscriberCount: subscriberCount,
	}, nil
}

// logWatcher polls SimLogDir for g5_*.log files and publishes new lines.
// Per-file byte offsets are tracked in a map; first open seeks to end (no
// replaying old history, only future lines).
func logWatcher(ctx context.Context, cfg config.Config, pub events.Publisher, m *gatewayMetrics) {
	offsets := make(map[string]int64)
	tick := time.NewTicker(500 * time.Millisecond)
	defer tick.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
			matches, _ := filepath.Glob(filepath.Join(cfg.SimLogDir, "g5_*.log"))
			for _, path := range matches {
				tailFile(path, offsets, pub, m)
			}
		}
	}
}

func tailFile(path string, offsets map[string]int64, pub events.Publisher, m *gatewayMetrics) {
	f, err := os.Open(path)
	if err != nil {
		return
	}
	defer f.Close()
	if _, known := offsets[path]; !known {
		end, _ := f.Seek(0, io.SeekEnd)
		offsets[path] = end
		return
	}
	if _, err := f.Seek(offsets[path], io.SeekStart); err != nil {
		return
	}
	sc := bufio.NewScanner(f)
	node := nodeFromPath(path)
	for sc.Scan() {
		if line := sc.Text(); line != "" {
			sev := "info"
			up := strings.ToUpper(line)
			if strings.Contains(up, "ERROR") || strings.Contains(up, "FAIL") {
				sev = "error"
			} else if strings.Contains(up, "WARN") {
				sev = "warn"
			}
			pub.Publish(events.SimEvent{ID: newID(), Timestamp: time.Now(), Node: node, EventType: events.EventLogLine, Detail: line, Severity: sev})
			// Record this event in the OTEL counter, labelled by which NF produced it.
			// In Prometheus this becomes: event_gateway_events_published_total{node="AMF"} 42
			m.eventsPublished.Add(context.Background(), 1,
				metric.WithAttributes(attribute.String("node", string(node))),
			)
		}
	}
	pos, _ := f.Seek(0, io.SeekCurrent)
	offsets[path] = pos
}

func nodeFromPath(path string) events.NodeType {
	base := strings.ToUpper(filepath.Base(path))
	switch {
	case strings.Contains(base, "AMF"):
		return events.NodeAMF
	case strings.Contains(base, "UDM"):
		return events.NodeUDM
	case strings.Contains(base, "SMF"):
		return events.NodeSMF
	case strings.Contains(base, "UPF"):
		return events.NodeUPF
	case strings.Contains(base, "GNB"):
		return events.NodeGNB
	default:
		return events.NodeNRF
	}
}

// nrfPoller queries the C++ NRF every 5s and publishes NF_REGISTERED /
// NF_DEREGISTERED events for changes since the last poll.
func nrfPoller(ctx context.Context, cfg config.Config, pub events.Publisher, m *gatewayMetrics) {
	tick := time.NewTicker(5 * time.Second)
	defer tick.Stop()
	known := make(map[string]bool)
	for {
		select {
		case <-ctx.Done():
			return
		case <-tick.C:
			current := fetchNFIDs(cfg.NRFAddr)
			for id := range current {
				if !known[id] {
					pub.Publish(events.SimEvent{ID: newID(), Timestamp: time.Now(), Node: events.NodeNRF, EventType: events.EventNFRegistered, Detail: "nfInstanceId=" + id, Severity: "info"})
					m.eventsPublished.Add(ctx, 1, metric.WithAttributes(attribute.String("node", "NRF")))
				}
			}
			for id := range known {
				if !current[id] {
					pub.Publish(events.SimEvent{ID: newID(), Timestamp: time.Now(), Node: events.NodeNRF, EventType: events.EventNFDeregistered, Detail: "nfInstanceId=" + id, Severity: "warn"})
					m.eventsPublished.Add(ctx, 1, metric.WithAttributes(attribute.String("node", "NRF")))
				}
			}
			known = current
		}
	}
}

func fetchNFIDs(nrfAddr string) map[string]bool {
	c := &http.Client{Timeout: 3 * time.Second}
	resp, err := c.Get(nrfAddr + "/nf-instances")
	if err != nil {
		return nil
	}
	defer resp.Body.Close()
	var profiles []struct {
		NfInstanceID string `json:"nfInstanceId"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&profiles); err != nil {
		return nil
	}
	m := make(map[string]bool, len(profiles))
	for _, p := range profiles {
		m[p.NfInstanceID] = true
	}
	return m
}

// sseStream handles GET /events/stream.
//
// Goroutine leak prevention: without defer store.Unsubscribe(subID), the channel
// would stay in the subscriber map forever, and the Publish goroutine would block
// trying to send to it — a goroutine leak. This is one of the most common Go bugs
// in SSE/WebSocket handlers. The defer below guarantees cleanup on any exit path.
func sseStream(store *events.BoundedStore, secret string, m *gatewayMetrics) http.Handler {
	return auth.RequireRole(secret, auth.RoleViewer, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		flusher, ok := w.(http.Flusher)
		if !ok {
			http.Error(w, "streaming unsupported", http.StatusInternalServerError)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		w.Header().Set("Cache-Control", "no-cache")
		w.Header().Set("X-Accel-Buffering", "no")

		subID, ch := store.Subscribe()
		// Track subscriber count with OTEL UpDownCounter (goes up on connect, down on disconnect).
		// Int64UpDownCounter is used (not Int64Counter) because subscribers can both increase
		// and decrease — a regular counter can only go up.
		m.subscriberCount.Add(r.Context(), 1)
		m.activeSubscribers.Add(1)
		defer func() {
			store.Unsubscribe(subID) // prevents goroutine leak
			m.subscriberCount.Add(r.Context(), -1)
			m.activeSubscribers.Add(-1)
		}()

		for {
			select {
			case <-r.Context().Done():
				return
			case ev, ok := <-ch:
				if !ok {
					return
				}
				data, _ := json.Marshal(ev)
				fmt.Fprintf(w, "data: %s\n\n", data)
				flusher.Flush()
			}
		}
	}))
}

func main() {
	cfg := config.Load()

	// ── Telemetry ─────────────────────────────────────────────────────────────
	shutdownTel, err := telemetry.Init(context.Background(), "event-gateway", cfg.OTLPEndpoint)
	if err != nil {
		slog.Error("telemetry init", "err", err)
		os.Exit(1)
	}

	// Create OTEL metric instruments. otel.GetMeterProvider() returns the
	// MeterProvider we set in telemetry.Init — the Prometheus bridge is attached.
	meter := otel.GetMeterProvider().Meter("event-gateway")
	gm, err := newGatewayMetrics(meter)
	if err != nil {
		slog.Error("metrics init", "err", err)
		os.Exit(1)
	}

	store := events.NewBoundedStore(1000)
	ctx, cancel := context.WithCancel(context.Background())

	var pub events.Publisher = store
	if cfg.KafkaBrokers != "" {
		kp := events.NewKafkaPublisher(cfg.KafkaBrokers, cfg.KafkaTopic)
		defer kp.Close()
		pub = &events.FanOutPublisher{Local: store, Kafka: kp}
		slog.Info("kafka fan-out enabled", "brokers", cfg.KafkaBrokers, "topic", cfg.KafkaTopic)
	}

	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); logWatcher(ctx, cfg, pub, gm) }()
	go func() { defer wg.Done(); nrfPoller(ctx, cfg, pub, gm) }()

	mux := http.NewServeMux()
	mux.HandleFunc("/health", health.Handler("event-gateway", nil))

	mux.Handle("/events", auth.RequireRole(cfg.JWTSecret, auth.RoleViewer, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		n := 50
		if q := r.URL.Query().Get("n"); q != "" {
			if v, err := strconv.Atoi(q); err == nil && v > 0 {
				n = v
			}
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(store.Recent(n))
	})))

	// Register /events/stream before /events so the longer path wins under
	// Go's default mux prefix-matching semantics.
	mux.Handle("/events/stream", sseStream(store, cfg.JWTSecret, gm))

	// otelhttp wraps the mux so every HTTP request to event-gateway gets a span.
	// Span names include the HTTP method + path, e.g. "GET /events/stream".
	handler := otelhttp.NewHandler(mux, "event-gateway",
		otelhttp.WithTracerProvider(otel.GetTracerProvider()),
	)

	addr := fmt.Sprintf(":%d", cfg.EventPort)
	srv := &http.Server{
		Addr:         addr,
		Handler:      handler,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 0, // SSE streams must not have a write deadline
		IdleTimeout:  60 * time.Second,
	}

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, os.Interrupt, syscall.SIGTERM)
	go func() {
		slog.Info("event-gateway starting", "addr", addr, "log_dir", cfg.SimLogDir)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("server error", "err", err)
			os.Exit(1)
		}
	}()

	<-quit
	slog.Info("shutting down event-gateway")
	cancel()
	shutCtx, shutCancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer shutCancel()
	_ = srv.Shutdown(shutCtx)
	wg.Wait()

	flushCtx, flushCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer flushCancel()
	shutdownTel(flushCtx)
	slog.Info("event-gateway stopped")
}
