// Package config loads all runtime configuration from environment variables.
//
// Why env-based config? This follows the 12-factor app methodology (factor III).
// Environment variables are the idiomatic injection point for K8s ConfigMaps and
// Secrets, Docker -e flags, and local .env files — no config-file format to parse,
// no path discovery, works identically in every environment.
package config

import (
	"os"
	"strconv"
)

// Config holds all service configuration. Zero value is not usable — call Load().
type Config struct {
	AuthPort    int    // AUTH_PORT: port the auth-service listens on
	ControlPort int    // CONTROL_PORT: port the control-api listens on
	EventPort   int    // EVENT_PORT: port the event-gateway listens on
	JWTSecret   string // JWT_SECRET: HMAC-SHA256 signing key (from K8s Secret, not ConfigMap)
	JWTExpiry   int    // JWT_EXPIRY_HOURS: token lifetime in hours
	NRFAddr     string // NRF_ADDR: base URL of the C++ NRF simulator
	SimBinDir   string // SIM_BIN_DIR: directory containing compiled C++ NF binaries
	SimLogDir   string // SIM_LOG_DIR: directory where C++ NFs write their log files

	// Kafka: empty KafkaBrokers disables Kafka entirely (event-gateway falls
	// back to the in-memory-only BoundedStore, kpi-consumer refuses to start).
	// This keeps local/no-Docker development working with zero setup.
	KafkaBrokers    string // KAFKA_BROKERS: comma-separated host:port list, e.g. "localhost:9092"
	KafkaTopic      string // KAFKA_TOPIC: topic event-gateway publishes SimEvents to
	KafkaDLQTopic   string // KAFKA_DLQ_TOPIC: dead-letter topic for events kpi-consumer couldn't process
	KafkaGroupID    string // KAFKA_GROUP_ID: consumer group ID for kpi-consumer
	KPIWindowSecs   int    // KPI_WINDOW_SECS: tumbling window size for KPI aggregation
	KPIConsumerPort int    // KPI_CONSUMER_PORT: port kpi-consumer's /health and /metrics listen on

	// Observability: OpenTelemetry + gRPC
	OTLPEndpoint string // OTLP_ENDPOINT: OTEL Collector gRPC address, e.g. "localhost:4317"
	GRPCAuthPort int    // GRPC_AUTH_PORT: port grpc-auth listens on
	GRPCAuthAddr string // GRPC_AUTH_ADDR: address of grpc-auth for client calls, e.g. "localhost:50051"
}

// Load reads configuration from environment variables, applying defaults where
// the variable is unset or empty. Call once at program startup.
func Load() Config {
	return Config{
		AuthPort:    envInt("AUTH_PORT", 8081),
		ControlPort: envInt("CONTROL_PORT", 8082),
		EventPort:   envInt("EVENT_PORT", 8083),
		JWTSecret:   envStr("JWT_SECRET", "dev-secret-CHANGE-IN-PROD"),
		JWTExpiry:   envInt("JWT_EXPIRY_HOURS", 24),
		NRFAddr:     envStr("NRF_ADDR", "http://localhost:29510"),
		SimBinDir:   envStr("SIM_BIN_DIR", "./build"),
		SimLogDir:   envStr("SIM_LOG_DIR", "/tmp"),

		KafkaBrokers:    envStr("KAFKA_BROKERS", ""),
		KafkaTopic:      envStr("KAFKA_TOPIC", "5g-sim-events"),
		KafkaDLQTopic:   envStr("KAFKA_DLQ_TOPIC", "5g-sim-events-dlq"),
		KafkaGroupID:    envStr("KAFKA_GROUP_ID", "kpi-readers"),
		KPIWindowSecs:   envInt("KPI_WINDOW_SECS", 60),
		KPIConsumerPort: envInt("KPI_CONSUMER_PORT", 8084),

		OTLPEndpoint: envStr("OTLP_ENDPOINT", ""),
		GRPCAuthPort: envInt("GRPC_AUTH_PORT", 50051),
		GRPCAuthAddr: envStr("GRPC_AUTH_ADDR", "localhost:50051"),
	}
}

// envStr returns the value of env var key, or fallback if the var is absent/empty.
func envStr(key, fallback string) string {
	if v := os.Getenv(key); v != "" {
		return v
	}
	return fallback
}

// envInt returns the integer value of env var key, or fallback on absence or
// parse failure. Ignores malformed values silently — enough for a simulator.
func envInt(key string, fallback int) int {
	if v := os.Getenv(key); v != "" {
		if n, err := strconv.Atoi(v); err == nil {
			return n
		}
	}
	return fallback
}
