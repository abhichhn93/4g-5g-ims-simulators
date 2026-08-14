// kpi-consumer: reads SimEvents from Kafka and computes a rolling per-NF
// health KPI (percentage of events that were NOT severity="error") over
// tumbling 1-minute windows.
//
// This is the consuming half of the Kafka fan-out event-gateway writes into
// (see internal/events/kafka.go). It is a separate binary/pod on purpose:
// event-gateway's job is ingestion + live SSE, kpi-consumer's job is
// aggregation — splitting them means kpi-consumer can be scaled, restarted,
// or fall behind independently without affecting the live event stream.
//
// Endpoints:
//
//	GET /health   — no auth (liveness/readiness probe target)
//	GET /metrics  — Prometheus (messages processed, DLQ count, consumer lag)
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/promauto"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/segmentio/kafka-go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/metric"

	"go-services/internal/config"
	"go-services/internal/events"
	"go-services/internal/health"
	"go-services/internal/telemetry"
)

// ── Prometheus metrics (promauto — registered in prometheus.DefaultRegisterer) ──
// These are the original Kafka consumer health counters.
var (
	messagesProcessed = promauto.NewCounter(prometheus.CounterOpts{
		Name: "kpi_consumer_messages_processed_total",
		Help: "Total events successfully handled without error, including idempotent no-ops for redelivered/duplicate event IDs.",
	})
	dlqTotal = promauto.NewCounter(prometheus.CounterOpts{
		Name: "kpi_consumer_dlq_total",
		Help: "Total events routed to the dead-letter topic after exhausting retries.",
	})
	consumerLag = promauto.NewGauge(prometheus.GaugeOpts{
		Name: "kpi_consumer_lag",
		Help: "Approximate number of unread messages behind the partition's latest offset, as reported by the client.",
	})
)

// ── OTEL metrics (via Prometheus bridge in telemetry.Init) ──────────────────
// These OTEL metrics flow through the bridge into the SAME DefaultRegisterer
// as the promauto counters above. In /metrics output they appear together:
//
//	# promauto counter
//	kpi_consumer_messages_processed_total 42
//	# OTEL counter (OTEL prefix style: underscores from dots)
//	kpi_consumer_window_closed_total{node="AMF"} 5
//	kpi_consumer_window_health_rate{node="AMF"} 97.5
//
// Interview point: "I used both in the same service to show they coexist.
// In a new service I'd pick OTEL metrics only — the bridge handles Prometheus."
var (
	otelWindowClosed    metric.Int64Counter   // kpi.consumer.window.closed{node=...}
	otelWindowHealthPct metric.Float64Gauge   // kpi.consumer.window.health_pct{node=...}
)

// windowKey identifies one tumbling window: one NF (Node) over one fixed
// time bucket. Aggregating per-Node (rather than one global counter) is what
// makes this useful as a KPI — "is AMF healthy" vs "is UPF healthy" are
// different questions with different on-call owners.
type windowKey struct {
	node  events.NodeType
	start time.Time
}

// windowStats is the running aggregate for one windowKey.
type windowStats struct {
	total   int
	errors  int
	printed bool // set once this window's KPI line has been emitted
	// seenIDs is the idempotency guard: see aggregator.ingest for why this
	// exists. Bounded in practice — one window holds at most a few hundred
	// event IDs before it closes and stops accepting new ones.
	seenIDs map[string]bool
}

// aggregator holds all open/recently-closed windows in memory, protected by
// a single mutex. Fine for this demo's throughput; a production version
// would shard by node or window to reduce lock contention.
type aggregator struct {
	mu         sync.Mutex
	windowSize time.Duration
	windows    map[windowKey]*windowStats
}

func newAggregator(windowSize time.Duration) *aggregator {
	return &aggregator{windowSize: windowSize, windows: make(map[windowKey]*windowStats)}
}

// ingest decodes one raw Kafka message and folds it into the correct window.
// Returns an error only for malformed input (bad JSON) — that's the one
// failure mode processWithRetry retries, and eventually DLQs.
func (a *aggregator) ingest(raw []byte) error {
	var e events.SimEvent
	if err := json.Unmarshal(raw, &e); err != nil {
		return fmt.Errorf("malformed event: %w", err)
	}

	// Tumbling window bucketing: truncate the event's timestamp down to the
	// nearest window boundary. With a 60s window, an event at 10:01:47 lands
	// in the bucket that started at 10:01:00 — Time.Truncate rounds down to
	// a multiple of the duration since the zero time, which for whole-minute
	// windows is exactly "start of this minute". "Tumbling" means windows are
	// fixed-size and non-overlapping (as opposed to a sliding window, where
	// each new event would open/extend multiple overlapping buckets).
	windowStart := e.Timestamp.Truncate(a.windowSize)
	key := windowKey{node: e.Node, start: windowStart}

	a.mu.Lock()
	defer a.mu.Unlock()

	w, ok := a.windows[key]
	if !ok {
		w = &windowStats{seenIDs: make(map[string]bool)}
		a.windows[key] = w
	}

	// Idempotency: Kafka is at-least-once delivery, not exactly-once. If this
	// consumer crashes after processing a message but before its offset
	// commit is flushed, a group rebalance replays that message from the last
	// committed offset — the SAME event.ID arrives again. Without this check,
	// a redeliver would double-count it and silently inflate the KPI (e.g.
	// showing 105% or an inflated attempt count). Keying the dedup set by the
	// event's own ID (not by message offset, which changes across restarts)
	// makes ingest() safe to call twice with an identical event.
	if w.seenIDs[e.ID] {
		return nil
	}
	w.seenIDs[e.ID] = true

	w.total++
	if e.Severity == "error" {
		w.errors++
	}
	return nil
}

// closeExpiredWindows runs on a ticker, emitting one KPI line the first time
// each window is seen to be past its end (+ a small grace period to absorb
// minor delivery lag) and marking it printed so it's never emitted twice.
// Windows are intentionally kept in memory (not deleted) after closing: a
// late/redelivered message for an already-closed window is still folded into
// its correct totals via ingest()'s idempotency check, it just won't trigger
// a second printed line — a deliberate simplification over a real stream
// processor's "update and re-emit" semantics, called out here so it doesn't
// read as an oversight.
func closeExpiredWindows(ctx context.Context, a *aggregator) {
	const grace = 3 * time.Second
	ticker := time.NewTicker(5 * time.Second)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			a.mu.Lock()
			for key, w := range a.windows {
				if !w.printed && now.After(key.start.Add(a.windowSize).Add(grace)) {
					w.printed = true
					printKPI(key.node, key.start, w)
				}
			}
			a.mu.Unlock()
		}
	}
}

func printKPI(node events.NodeType, windowStart time.Time, w *windowStats) {
	rate := 100.0
	if w.total > 0 {
		rate = 100.0 * float64(w.total-w.errors) / float64(w.total)
	}
	fmt.Printf("[%s] %s health: %.0f%% ok (%d events)\n", windowStart.Format("15:04:05"), node, rate, w.total)

	// Record OTEL metrics for this closed window.
	// otelWindowClosed and otelWindowHealthPct are OTEL instruments;
	// the Prometheus bridge publishes them at /metrics alongside promauto counters.
	ctx := context.Background()
	nodeAttr := metric.WithAttributes(attribute.String("node", string(node)))
	if otelWindowClosed != nil {
		otelWindowClosed.Add(ctx, 1, nodeAttr)
	}
	if otelWindowHealthPct != nil {
		otelWindowHealthPct.Record(ctx, rate, nodeAttr)
	}
}

// processWithRetry attempts a.ingest up to 4 times total (1 initial + 3
// retries) with exponential backoff between attempts, before giving up and
// routing the raw message to the DLQ topic.
//
// Why a DLQ instead of just logging and dropping: this consumer reads one
// partition sequentially. If we simply `continue`d past a bad message, that's
// fine — but if we instead blocked/crashed on it (e.g. a naive implementation
// that panics on unmarshal error), every message behind it in the partition
// would be stuck forever, since Kafka only delivers a partition's messages in
// order to one consumer at a time. A DLQ lets us acknowledge and move past
// the poison message immediately after exhausting retries, while preserving
// it (rather than silently dropping) so it can be inspected/replayed later.
func processWithRetry(dlq *kafka.Writer, key, raw []byte, a *aggregator) {
	backoffs := []time.Duration{1 * time.Second, 2 * time.Second, 4 * time.Second}

	var lastErr error
	for attempt := 0; ; attempt++ {
		if err := a.ingest(raw); err != nil {
			lastErr = err
			if attempt >= len(backoffs) {
				break
			}
			slog.Warn("kpi-consumer: processing failed, retrying", "attempt", attempt+1, "err", err)
			time.Sleep(backoffs[attempt])
			continue
		}
		messagesProcessed.Inc()
		return
	}

	dlqTotal.Inc()
	slog.Error("kpi-consumer: giving up after retries, routing to DLQ", "err", lastErr)
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := dlq.WriteMessages(ctx, kafka.Message{Key: key, Value: raw}); err != nil {
		slog.Error("kpi-consumer: failed to write to DLQ too", "err", err)
	}
}

func main() {
	cfg := config.Load()

	// ── Telemetry ─────────────────────────────────────────────────────────────
	// telemetry.Init registers the Prometheus bridge in DefaultRegisterer.
	// After this, OTEL metrics (otelWindowClosed etc.) appear in the SAME
	// /metrics response as the promauto counters above — one endpoint, both systems.
	shutdownTel, err := telemetry.Init(context.Background(), "kpi-consumer", cfg.OTLPEndpoint)
	if err != nil {
		slog.Error("telemetry init", "err", err)
		os.Exit(1)
	}

	meter := otel.GetMeterProvider().Meter("kpi-consumer")
	otelWindowClosed, err = meter.Int64Counter(
		"kpi.consumer.window.closed",
		metric.WithDescription("Number of 1-minute KPI windows closed, by NF node"),
		metric.WithUnit("{window}"),
	)
	if err != nil {
		slog.Error("otel counter init", "err", err)
		os.Exit(1)
	}
	otelWindowHealthPct, err = meter.Float64Gauge(
		"kpi.consumer.window.health_pct",
		metric.WithDescription("Health percentage (0-100) for the last closed window, by NF node"),
		metric.WithUnit("%"),
	)
	if err != nil {
		slog.Error("otel gauge init", "err", err)
		os.Exit(1)
	}

	if cfg.KafkaBrokers == "" {
		slog.Error("KAFKA_BROKERS is not set — kpi-consumer has nothing to read from")
		os.Exit(1)
	}
	brokers := strings.Split(cfg.KafkaBrokers, ",")

	// GroupID is what makes this a consumer *group* member rather than a
	// standalone reader: Kafka tracks committed offsets per (GroupID, Topic,
	// Partition), and if you ran a second kpi-consumer process with the same
	// GroupID, Kafka would split the topic's partitions between the two —
	// each partition still goes to exactly one group member at a time, so
	// work is shared, not duplicated. See docs/KAFKA_INTERVIEW_PREP.md.
	kgoLogger := kafka.LoggerFunc(func(format string, args ...interface{}) { slog.Debug("kafka-go: " + fmt.Sprintf(format, args...)) })
	kgoErrLogger := kafka.LoggerFunc(func(format string, args ...interface{}) { slog.Error("kafka-go: " + fmt.Sprintf(format, args...)) })
	reader := kafka.NewReader(kafka.ReaderConfig{
		Brokers:     brokers,
		Topic:       cfg.KafkaTopic,
		GroupID:     cfg.KafkaGroupID,
		Logger:      kgoLogger,
		ErrorLogger: kgoErrLogger,
	})
	defer reader.Close()

	dlqWriter := &kafka.Writer{
		Addr:                   kafka.TCP(brokers...),
		Topic:                  cfg.KafkaDLQTopic,
		RequiredAcks:           kafka.RequireOne,
		AllowAutoTopicCreation: true, // belt-and-suspenders: docker/docker-compose.kafka.yml's kafka-init already pre-creates this topic
	}
	defer dlqWriter.Close()

	agg := newAggregator(time.Duration(cfg.KPIWindowSecs) * time.Second)

	ctx, cancel := context.WithCancel(context.Background())
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, os.Interrupt, syscall.SIGTERM)
	go func() { <-quit; slog.Info("kpi-consumer: shutdown signal received"); cancel() }()

	go closeExpiredWindows(ctx, agg)

	mux := http.NewServeMux()
	mux.HandleFunc("/health", health.Handler("kpi-consumer", nil))
	mux.Handle("/metrics", promhttp.Handler())
	srv := &http.Server{Addr: fmt.Sprintf(":%d", cfg.KPIConsumerPort), Handler: mux}
	go func() {
		slog.Info("kpi-consumer starting", "metrics_addr", srv.Addr, "topic", cfg.KafkaTopic, "group", cfg.KafkaGroupID)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("kpi-consumer: metrics server error", "err", err)
		}
	}()

	for {
		m, err := reader.ReadMessage(ctx)
		if err != nil {
			if ctx.Err() != nil {
				break // context cancelled: normal shutdown, not an error
			}
			slog.Error("kpi-consumer: read failed", "err", err)
			continue
		}
		consumerLag.Set(float64(reader.Stats().Lag))
		processWithRetry(dlqWriter, m.Key, m.Value, agg)
	}

	shutCtx, shutCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutCancel()
	_ = srv.Shutdown(shutCtx)

	flushCtx, flushCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer flushCancel()
	shutdownTel(flushCtx)
	slog.Info("kpi-consumer stopped")
}
