// auth-service: issues and validates JWT tokens for the Go microservice layer.
//
// Endpoints:
//
//	POST /login  — returns a signed JWT given valid username+password
//	GET  /me     — returns the caller's username and role (requires Bearer token)
//	GET  /health — liveness/readiness probe, no auth required
//
// Observability added (OpenTelemetry):
//
//	Every HTTP request gets an automatic span via otelhttp.NewHandler().
//	The /login handler adds CHILD spans for the two slow operations:
//	  - bcrypt.compare   (CPU-intensive: ~100ms at cost 12)
//	  - jwt.issue        (~1ms: HMAC-SHA256 sign)
//	These child spans appear in Jaeger as nested bars under the parent HTTP span.
//
// In production: replace the hardcoded user map with LDAP/OAuth2 lookup.
package main

import (
	"context"
	"encoding/json"
	"fmt"
	"log/slog"
	"net/http"
	"os"
	"os/signal"
	"syscall"
	"time"

	"go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp"
	"go.opentelemetry.io/otel"
	otelcodes "go.opentelemetry.io/otel/codes"
	"golang.org/x/crypto/bcrypt"

	"go-services/internal/auth"
	"go-services/internal/config"
	"go-services/internal/health"
	"go-services/internal/telemetry"
)

// user holds a bcrypt-hashed password and the role granted on successful login.
type user struct {
	hash []byte
	role auth.Role
}

// buildUserDB creates the demo user database at startup.
// Passwords are hashed with bcrypt (cost 12) — never stored in plain text.
//
// Why bcrypt? It is adaptive (cost factor slows brute-force as CPUs get faster),
// resistant to rainbow tables (built-in random salt per hash), and is the
// standard recommendation for password storage (OWASP, RFC 9106).
func buildUserDB() map[string]user {
	mustHash := func(plain string) []byte {
		h, err := bcrypt.GenerateFromPassword([]byte(plain), 12)
		if err != nil {
			panic(fmt.Sprintf("bcrypt hash failed: %v", err))
		}
		return h
	}
	return map[string]user{
		"admin":  {hash: mustHash("admin123"), role: auth.RoleAdmin},
		"viewer": {hash: mustHash("view123"), role: auth.RoleViewer},
	}
}

// logMiddleware wraps an http.Handler, logging method, path, status, and latency
// as structured JSON via slog. This is the simplest possible access log.
func logMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		start := time.Now()
		rw := &statusWriter{ResponseWriter: w, status: http.StatusOK}
		next.ServeHTTP(rw, r)
		slog.Info("request",
			"method", r.Method,
			"path", r.URL.Path,
			"status", rw.status,
			"latency_ms", time.Since(start).Milliseconds(),
		)
	})
}

// statusWriter captures the HTTP status code written by the handler so the
// logging middleware can record it after the fact.
type statusWriter struct {
	http.ResponseWriter
	status int
}

func (sw *statusWriter) WriteHeader(code int) {
	sw.status = code
	sw.ResponseWriter.WriteHeader(code)
}

func main() {
	cfg := config.Load()
	users := buildUserDB()

	// ── Telemetry ─────────────────────────────────────────────────────────────
	// Init returns a shutdown func that flushes all buffered spans on exit.
	shutdownTel, err := telemetry.Init(context.Background(), "auth-service", cfg.OTLPEndpoint)
	if err != nil {
		slog.Error("telemetry init", "err", err)
		os.Exit(1)
	}

	// tracer is used in handlers to create child spans under the otelhttp parent.
	tracer := otel.Tracer("auth-service")

	mux := http.NewServeMux()

	// POST /login — no auth required
	mux.HandleFunc("/login", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodPost {
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}
		var body struct {
			Username string `json:"username"`
			Password string `json:"password"`
		}
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			http.Error(w, "invalid JSON body", http.StatusBadRequest)
			return
		}

		u, ok := users[body.Username]
		if !ok {
			http.Error(w, "invalid credentials", http.StatusUnauthorized)
			return
		}

		// ── bcrypt span ────────────────────────────────────────────────────
		// tracer.Start() creates a child span of the HTTP span otelhttp created.
		// In Jaeger you'll see:
		//   HTTP POST /login          ← parent (otelhttp, automatic)
		//     └─ bcrypt.compare       ← this span (~100ms)
		//     └─ jwt.issue            ← next span (~1ms)
		_, bcryptSpan := tracer.Start(r.Context(), "bcrypt.compare")
		bcryptErr := bcrypt.CompareHashAndPassword(u.hash, []byte(body.Password))
		if bcryptErr != nil {
			bcryptSpan.RecordError(bcryptErr)
			bcryptSpan.SetStatus(otelcodes.Error, "password mismatch")
			bcryptSpan.End()
			http.Error(w, "invalid credentials", http.StatusUnauthorized)
			return
		}
		bcryptSpan.SetStatus(otelcodes.Ok, "")
		bcryptSpan.End()

		// ── jwt.issue span ─────────────────────────────────────────────────
		_, jwtSpan := tracer.Start(r.Context(), "jwt.issue")
		token, err := auth.Issue(cfg.JWTSecret, body.Username, u.role, cfg.JWTExpiry)
		if err != nil {
			jwtSpan.RecordError(err)
			jwtSpan.SetStatus(otelcodes.Error, "sign failed")
			jwtSpan.End()
			slog.Error("token issue failed", "err", err)
			http.Error(w, "internal error", http.StatusInternalServerError)
			return
		}
		jwtSpan.SetStatus(otelcodes.Ok, "")
		jwtSpan.End()

		w.Header().Set("Content-Type", "application/json")
		_ = json.NewEncoder(w).Encode(map[string]any{
			"token":      token,
			"expires_in": cfg.JWTExpiry * 3600,
		})
	})

	// GET /me — requires any valid token (viewer minimum)
	mux.Handle("/me", auth.RequireRole(cfg.JWTSecret, auth.RoleViewer,
		http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
			claims, _ := auth.ClaimsFromContext(r.Context())
			w.Header().Set("Content-Type", "application/json")
			_ = json.NewEncoder(w).Encode(map[string]string{
				"username": claims.Username,
				"role":     string(claims.Role),
			})
		}),
	))

	// GET /health — no auth, used by K8s readiness + liveness probes
	mux.HandleFunc("/health", health.Handler("auth-service", nil))

	// otelhttp.NewHandler wraps the mux so every request gets a span automatically.
	// The span name is "auth-service" (the second arg); otelhttp adds the HTTP
	// method and route as span attributes. No per-handler boilerplate needed.
	otelHandler := otelhttp.NewHandler(logMiddleware(mux), "auth-service",
		otelhttp.WithTracerProvider(otel.GetTracerProvider()),
	)

	addr := fmt.Sprintf(":%d", cfg.AuthPort)
	srv := &http.Server{
		Addr:         addr,
		Handler:      otelHandler,
		ReadTimeout:  10 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  60 * time.Second,
	}

	// Graceful shutdown: wait for SIGINT or SIGTERM before stopping.
	// In K8s, SIGTERM is sent when a pod is deleted; we have until
	// terminationGracePeriodSeconds to finish in-flight requests.
	quit := make(chan os.Signal, 1)
	signal.Notify(quit, os.Interrupt, syscall.SIGTERM)

	go func() {
		slog.Info("auth-service starting", "addr", addr)
		if err := srv.ListenAndServe(); err != nil && err != http.ErrServerClosed {
			slog.Error("server error", "err", err)
			os.Exit(1)
		}
	}()

	<-quit
	slog.Info("shutting down auth-service")
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	if err := srv.Shutdown(ctx); err != nil {
		slog.Error("shutdown error", "err", err)
	}

	// Flush spans AFTER HTTP server drains so in-flight handler spans finish
	// before we close the exporter connection.
	shutCtx, shutCancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer shutCancel()
	shutdownTel(shutCtx)
	slog.Info("auth-service stopped")
}
