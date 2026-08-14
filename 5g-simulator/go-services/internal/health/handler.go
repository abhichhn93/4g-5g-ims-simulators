// Package health provides a standardized /health endpoint for all Go services.
// The response shape is simple JSON: K8s readiness/liveness probes just check
// the HTTP 200 status code; the body is for human operators and dashboards.
package health

import (
	"encoding/json"
	"net/http"
	"time"
)

// Status is the JSON body returned by /health.
type Status struct {
	Status  string            `json:"status"`           // "ok" or "degraded"
	Service string            `json:"service"`          // service name for easy identification
	Uptime  string            `json:"uptime"`           // human-readable, e.g. "2h3m4s"
	Checks  map[string]string `json:"checks,omitempty"` // optional named sub-checks
}

// Handler returns an http.HandlerFunc for GET /health.
//
// extra is a map of check-name → func() string. Each function is called on
// every request and its return value appears in Checks. If any check returns
// a value starting with "error", the overall status becomes "degraded".
//
// start time is captured at the call to Handler (service startup), so Uptime
// measures how long the service has been alive, not how long since last request.
func Handler(serviceName string, extra map[string]func() string) http.HandlerFunc {
	start := time.Now() // captured once at registration time

	return func(w http.ResponseWriter, r *http.Request) {
		checks := make(map[string]string, len(extra))
		overall := "ok"

		for name, fn := range extra {
			result := fn()
			checks[name] = result
			if len(result) >= 5 && result[:5] == "error" {
				overall = "degraded"
			}
		}

		s := Status{
			Status:  overall,
			Service: serviceName,
			Uptime:  time.Since(start).Round(time.Second).String(),
		}
		if len(checks) > 0 {
			s.Checks = checks
		}

		w.Header().Set("Content-Type", "application/json")
		// Always 200: K8s kills the pod on non-2xx liveness; we prefer to let
		// "degraded" be visible without triggering an immediate restart storm.
		w.WriteHeader(http.StatusOK)
		_ = json.NewEncoder(w).Encode(s)
	}
}
