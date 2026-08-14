// Package telemetry wires OpenTelemetry into the Go microservice layer.
//
// Two pipelines in one Init call:
//
//   TRACES:  TracerProvider → BatchSpanProcessor → OTLP gRPC → OTEL Collector → Jaeger
//   METRICS: MeterProvider  → Prometheus bridge  → same /metrics endpoint as promauto
//
// Every service calls telemetry.Init() once in main(), then uses:
//
//	tracer := otel.Tracer("service-name")
//	ctx, span := tracer.Start(ctx, "operation-name")
//	defer span.End()
//
// HTTP spans are automatically created by wrapping the mux with otelhttp.NewHandler().
// gRPC spans are created by the UnaryTraceInterceptor in cmd/grpc-auth/main.go.
package telemetry

import (
	"context"
	"fmt"
	"log/slog"

	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/exporters/otlp/otlptrace/otlptracegrpc"
	otelprom "go.opentelemetry.io/otel/exporters/prometheus"
	"go.opentelemetry.io/otel/propagation"
	sdkmetric "go.opentelemetry.io/otel/sdk/metric"
	"go.opentelemetry.io/otel/sdk/resource"
	sdktrace "go.opentelemetry.io/otel/sdk/trace"
	semconv "go.opentelemetry.io/otel/semconv/v1.26.0"
	"google.golang.org/grpc"
	"google.golang.org/grpc/credentials/insecure"
)

// Init initialises both the trace and metric pipelines.
//
// serviceName appears as "service.name" on every span and metric — Jaeger uses
// this to colour-code which service emitted a span in the waterfall view.
//
// otlpEndpoint is the OTEL Collector's gRPC receiver, e.g. "localhost:4317".
// If empty, Init returns (nil, nil) and is a no-op: all otel.Tracer() calls
// return a no-op tracer, so the services still compile and run without a
// collector running.
//
// The returned shutdown must be called before process exit (defer it in main).
// It flushes all buffered spans to the collector so nothing is lost on shutdown.
func Init(ctx context.Context, serviceName, otlpEndpoint string) (shutdown func(context.Context), err error) {
	if otlpEndpoint == "" {
		slog.Info("telemetry: OTLP_ENDPOINT not set — running with no-op tracer")
		return func(context.Context) {}, nil
	}

	// Resource: a bundle of key=value pairs stamped on every span and metric
	// emitted from this process. Backends use these for filtering and grouping.
	// resource.Default() adds host.name, process.pid, os.type automatically.
	res, err := resource.Merge(
		resource.Default(),
		resource.NewWithAttributes(
			semconv.SchemaURL,
			semconv.ServiceName(serviceName),
			semconv.DeploymentEnvironment("dev"),
		),
	)
	if err != nil {
		return nil, fmt.Errorf("telemetry resource: %w", err)
	}

	// ─── TRACE PIPELINE ─────────────────────────────────────────────────────
	//
	// grpc.NewClient opens a gRPC connection to the OTEL Collector.
	// The Collector is the hub: it receives OTLP spans from ALL services,
	// batches them, and fans out to Jaeger (traces) and Prometheus (metrics).
	//
	// Why a Collector instead of exporting directly to Jaeger?
	// Decoupling: the Collector can be reconfigured (add a second backend,
	// change sampling rate, add tail-based sampling) without redeploying services.
	grpcConn, err := grpc.NewClient(
		otlpEndpoint,
		grpc.WithTransportCredentials(insecure.NewCredentials()),
	)
	if err != nil {
		return nil, fmt.Errorf("telemetry grpc conn to collector: %w", err)
	}

	traceExporter, err := otlptracegrpc.New(ctx,
		otlptracegrpc.WithGRPCConn(grpcConn),
	)
	if err != nil {
		return nil, fmt.Errorf("telemetry trace exporter: %w", err)
	}

	tp := sdktrace.NewTracerProvider(
		// BatchSpanProcessor: collects spans in memory and sends them in bulk
		// every 5s (or when the 512-span buffer is full). Sending one HTTP
		// call per span would saturate the network at production throughput.
		sdktrace.WithBatcher(traceExporter),
		sdktrace.WithResource(res),
		// AlwaysSample traces every request — fine for dev/demo.
		// Production: use sdktrace.TraceIDRatioBased(0.01) to sample 1%.
		sdktrace.WithSampler(sdktrace.AlwaysSample()),
	)

	// Register as the global provider so otel.Tracer("name") works anywhere.
	otel.SetTracerProvider(tp)

	// W3C TraceContext propagation: when auth-service calls grpc-auth, it injects
	// the current span's trace ID into the HTTP/gRPC headers as "traceparent".
	// The receiving service extracts it and continues the SAME trace instead of
	// starting a new one. Without this, you'd see isolated spans per service.
	otel.SetTextMapPropagator(propagation.NewCompositeTextMapPropagator(
		propagation.TraceContext{}, // W3C traceparent/tracestate headers
		propagation.Baggage{},      // W3C Baggage (arbitrary k=v along the trace)
	))

	// ─── METRIC PIPELINE ────────────────────────────────────────────────────
	//
	// The Prometheus exporter creates a Prometheus Collector that registers
	// itself in prometheus.DefaultRegisterer. This means OTEL counters/gauges
	// appear in the SAME /metrics response as the promauto counters in
	// kpi-consumer — no second scrape target, no Grafana datasource change.
	//
	// Interview hook: "OTEL metrics and Prometheus are two different systems,
	// but the bridge means you don't have to choose — Prometheus scrapes
	// everything from one /metrics endpoint."
	promExporter, err := otelprom.New()
	if err != nil {
		return nil, fmt.Errorf("telemetry prometheus exporter: %w", err)
	}

	mp := sdkmetric.NewMeterProvider(
		sdkmetric.WithReader(promExporter),
		sdkmetric.WithResource(res),
	)
	otel.SetMeterProvider(mp)

	slog.Info("telemetry initialized",
		"service", serviceName,
		"otlp_endpoint", otlpEndpoint,
	)

	return func(ctx context.Context) {
		// Shutdown flushes all in-flight spans to the collector. Without this,
		// spans created in the last ~5s before process exit would be lost.
		if err := tp.Shutdown(ctx); err != nil {
			slog.Error("trace provider shutdown error", "err", err)
		}
		if err := mp.Shutdown(ctx); err != nil {
			slog.Error("meter provider shutdown error", "err", err)
		}
		_ = grpcConn.Close()
	}, nil
}
