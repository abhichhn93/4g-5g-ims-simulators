// control-api: manages C++ simulator lifecycle and proxies NRF discovery.
//
// Endpoints:
//
//	GET  /health            — no auth
//	GET  /metrics           — Prometheus text (role: viewer)
//	GET  /scenarios         — status + NF list from NRF (role: viewer)
//	POST /scenarios/start   — launch C++ NFs (role: operator)
//	POST /scenarios/stop    — stop C++ NFs (role: operator)
//
// Observability added (OpenTelemetry):
//
//	scenario.start span — covers the full NF launch sequence.
//	  attributes: nf_count (how many NFs launched), sim_bin_dir
//	  child span per NF binary: nf.launch{nf="amf_sim"} etc.
//	scenario.stop span — covers the SIGTERM + wait sequence.
//
// In production K8s: replace exec.Command with client-go Deployment patches.
// See k8s/control-api-deployment.yaml for the full rationale.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"log/slog"
	"net/http"
	"os"
	"os/exec"
	"os/signal"
	"path/filepath"
	"sync"
	"sync/atomic"
	"syscall"
	"time"

	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	otelcodes "go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/trace"

	"go-services/internal/auth"
	"go-services/internal/config"
	"go-services/internal/health"
	"go-services/internal/telemetry"
)

// nfBinaries: nrf_sim first — others register with NRF on startup.
var nfBinaries = []string{"nrf_sim", "udm_sim", "upf_sim", "smf_sim", "amf_sim"}

type ScenarioState struct {
	mu        sync.Mutex
	status    string // "idle" | "starting" | "running" | "stopping"
	procs     []*exec.Cmd
	startedAt time.Time
	tracer    trace.Tracer
}

func (s *ScenarioState) start(ctx context.Context, cfg config.Config) error {
	// scenario.start span: covers the entire NF launch sequence.
	// The context carries the traceparent from the HTTP handler's span
	// (created by otelhttp), so this span appears as a CHILD in Jaeger.
	ctx, span := s.tracer.Start(ctx, "scenario.start",
		trace.WithAttributes(attribute.String("sim_bin_dir", cfg.SimBinDir)),
	)
	defer span.End()

	s.mu.Lock()
	if s.status == "running" || s.status == "starting" {
		s.mu.Unlock()
		span.SetStatus(otelcodes.Error, "already "+s.status)
		return fmt.Errorf("scenario already %s", s.status)
	}
	s.status = "starting"
	s.mu.Unlock()

	// Each NF binary gets its own child span so we can see in Jaeger exactly
	// which binary was slow to start (or failed).
	launch := func(bin string) (*exec.Cmd, error) {
		_, nfSpan := s.tracer.Start(ctx, "nf.launch",
			trace.WithAttributes(attribute.String("nf", bin)),
		)
		defer nfSpan.End()

		cmd := exec.Command(filepath.Join(cfg.SimBinDir, bin))
		cmd.Stdout, cmd.Stderr = os.Stdout, os.Stderr
		if err := cmd.Start(); err != nil {
			nfSpan.RecordError(err)
			nfSpan.SetStatus(otelcodes.Error, err.Error())
			return nil, err
		}
		nfSpan.SetStatus(otelcodes.Ok, "")
		return cmd, nil
	}

	nrfCmd, err := launch("nrf_sim")
	if err != nil {
		s.mu.Lock()
		s.status = "idle"
		s.mu.Unlock()
		span.RecordError(err)
		span.SetStatus(otelcodes.Error, "nrf_sim failed to start")
		return fmt.Errorf("starting nrf_sim: %w", err)
	}
	time.Sleep(time.Second)

	cmds := []*exec.Cmd{nrfCmd}
	for _, bin := range nfBinaries[1:] {
		if cmd, err := launch(bin); err != nil {
			slog.Warn("failed to start NF", "bin", bin, "err", err)
		} else {
			cmds = append(cmds, cmd)
		}
	}

	s.mu.Lock()
	s.procs, s.status, s.startedAt = cmds, "running", time.Now()
	s.mu.Unlock()

	// Record the final NF count as a span attribute for easy filtering in Jaeger.
	span.SetAttributes(attribute.Int("nf_count", len(cmds)))
	span.SetStatus(otelcodes.Ok, "")
	slog.Info("scenario started", "nf_count", len(cmds))
	return nil
}

func (s *ScenarioState) stop(ctx context.Context) {
	// scenario.stop span covers SIGTERM + wait (or SIGKILL on timeout).
	// Using context.Background() as parent because stop() can be called from
	// the shutdown signal handler (no HTTP request context available then).
	_, span := s.tracer.Start(ctx, "scenario.stop")
	defer span.End()

	s.mu.Lock()
	if s.status != "running" {
		s.mu.Unlock()
		span.SetStatus(otelcodes.Ok, "nothing to stop")
		return
	}
	s.status = "stopping"
	procs := s.procs
	s.mu.Unlock()

	for _, cmd := range procs {
		if cmd.Process != nil {
			_ = cmd.Process.Signal(syscall.SIGTERM)
		}
	}
	done := make(chan struct{})
	go func() {
		for _, c := range procs {
			_ = c.Wait()
		}
		close(done)
	}()
	select {
	case <-done:
		slog.Info("all NFs stopped gracefully")
		span.SetAttributes(attribute.Bool("graceful", true))
		span.SetStatus(otelcodes.Ok, "graceful shutdown")
	case <-time.After(3 * time.Second):
		slog.Warn("timeout; sending SIGKILL")
		span.SetAttributes(attribute.Bool("graceful", false))
		span.AddEvent("sigkill_sent") // span event: a timestamped annotation on the span
		for _, c := range procs {
			if c.Process != nil {
				_ = c.Process.Signal(syscall.SIGKILL)
			}
		}
		span.SetStatus(otelcodes.Error, "forced kill after timeout")
	}
	s.mu.Lock()
	s.procs = nil
	s.status = "idle"
	s.mu.Unlock()
}

// requestCounters: path → atomic int64, lock-free request counting.
// In production replace with prometheus/client_golang histograms.
var requestCounters sync.Map

func incCounter(path string) {
	v, _ := requestCounters.LoadOrStore(path, new(atomic.Int64))
	v.(*atomic.Int64).Add(1)
}

func metricsText(running bool) string {
	flag := 0
	if running {
		flag = 1
	}
	out := fmt.Sprintf("# HELP go_scenario_status Current scenario status (1=running)\n# TYPE go_scenario_status gauge\ngo_scenario_status %d\n# HELP go_requests_total Total HTTP requests\n# TYPE go_requests_total counter\n", flag)
	requestCounters.Range(func(k, v any) bool {
		out += fmt.Sprintf("go_requests_total{path=%q} %d\n", k, v.(*atomic.Int64).Load())
		return true
	})
	return out
}

func fetchNRF(nrfAddr string) (json.RawMessage, bool) {
	c := &http.Client{Timeout: 3 * time.Second}
	resp, err := c.Get(nrfAddr + "/nf-instances")
	if err != nil {
		return nil, false
	}
	defer resp.Body.Close()
	data, _ := io.ReadAll(resp.Body)
	return data, true
}

func main() {
	cfg := config.Load()

	// ── Telemetry ─────────────────────────────────────────────────────────────
	shutdownTel, err := telemetry.Init(context.Background(), "control-api", cfg.OTLPEndpoint)
	if err != nil {
		slog.Error("telemetry init", "err", err)
		os.Exit(1)
	}

	tracer := otel.Tracer("control-api")
	state := &ScenarioState{status: "idle", tracer: tracer}
	mux := http.NewServeMux()

	mux.HandleFunc("/health", health.Handler("control-api", map[string]func() string{
		"scenario": func() string { state.mu.Lock(); defer state.mu.Unlock(); return state.status },
	}))

	mux.Handle("/metrics", auth.RequireRole(cfg.JWTSecret, auth.RoleViewer, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		incCounter("/metrics")
		state.mu.Lock()
		running := state.status == "running"
		state.mu.Unlock()
		w.Header().Set("Content-Type", "text/plain; version=0.0.4")
		fmt.Fprint(w, metricsText(running))
	})))

	mux.Handle("/scenarios", auth.RequireRole(cfg.JWTSecret, auth.RoleViewer, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		incCounter("/scenarios")
		state.mu.Lock()
		status, uptime := state.status, ""
		if state.status == "running" {
			uptime = time.Since(state.startedAt).Round(time.Second).String()
		}
		state.mu.Unlock()
		nfData, reachable := fetchNRF(cfg.NRFAddr)
		if !reachable {
			nfData = json.RawMessage("[]")
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]any{"status": status, "uptime": uptime, "nrf_reachable": reachable, "nf_instances": nfData})
	})))

	mux.Handle("/scenarios/start", auth.RequireRole(cfg.JWTSecret, auth.RoleOperator, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		// Pass r.Context() so scenario.start span is a child of the HTTP span
		// otelhttp created — the full chain appears in Jaeger as one waterfall.
		if err := state.start(r.Context(), cfg); err != nil {
			http.Error(w, err.Error(), http.StatusConflict)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{"status": "starting"})
	})))

	mux.Handle("/scenarios/stop", auth.RequireRole(cfg.JWTSecret, auth.RoleOperator, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		go state.stop(context.Background()) // detached: don't cancel when HTTP response returns
		w.Header().Set("Content-Type", "application/json")
		json.NewEncoder(w).Encode(map[string]string{"status": "stopping"})
	})))

	// otelhttp wraps the mux so every HTTP request to control-api gets a span.
	// The /scenarios/start span then links to the scenario.start child span.
	otelMux := otelhttp.NewHandler(mux, "control-api",
		otelhttp.WithTracerProvider(otel.GetTracerProvider()),
	)

	addr := fmt.Sprintf(":%d", cfg.ControlPort)
	srv := &http.Server{Addr: addr, Handler: otelMux, ReadTimeout: 10 * time.Second, WriteTimeout: 30 * time.Second, IdleTimeout: 60 * time.Second}

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, os.Interrupt, syscall.SIGTERM)
	go func() {
		slog.Info("control-api starting", "addr", addr, "nrf", cfg.NRFAddr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("server error", "err", err)
			os.Exit(1)
		}
	}()

	<-quit
	slog.Info("shutting down control-api")
	state.stop(context.Background())
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		slog.Error("shutdown error", "err", err)
	}

	flushCtx, flushCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer flushCancel()
	shutdownTel(flushCtx)
	slog.Info("control-api stopped")
}
