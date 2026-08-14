// grpc-auth: standalone gRPC server that validates JWT tokens for other services.
//
// Why a separate binary?
//   Centralises token-verification logic (no duplicate auth.Verify calls).
//   Scales independently from HTTP services.
//   Demonstrates service-to-service gRPC: the pattern used in real 5G Core SBI.
//
// How it works:
//
//	client ──HTTP /login──▶ auth-service :8081  (issues JWT)
//	other service  ──gRPC CheckToken──▶ grpc-auth :50051  (validates JWT)
//
// The gRPC server has two chained interceptors (outermost executes first):
//
//	UnaryTraceInterceptor  →  UnaryLogInterceptor  →  handler
//	      (spans)                  (access log)         (logic)
//
// Server reflection is enabled so grpcurl works without needing the .proto file:
//
//	grpcurl -plaintext localhost:50051 list
//	grpcurl -plaintext -d '{"token":"<jwt>"}' localhost:50051 auth.AuthService/CheckToken
package main

import (
	"context"
	"fmt"
	"log/slog"
	"net"
	"os"
	"os/signal"
	"syscall"
	"time"

	"go.opentelemetry.io/otel"
	"google.golang.org/grpc"
	"google.golang.org/grpc/reflection"

	"go-services/internal/config"
	authgrpc "go-services/internal/grpc"
	"go-services/internal/grpc/authpb"
	"go-services/internal/telemetry"
)

func main() {
	cfg := config.Load()

	// ── Telemetry ────────────────────────────────────────────────────────────
	// telemetry.Init sets the global TracerProvider + MeterProvider.
	// After this call, otel.Tracer("grpc-auth") returns a real tracer that
	// sends spans to the OTEL Collector.
	shutdownTel, err := telemetry.Init(context.Background(), "grpc-auth", cfg.OTLPEndpoint)
	if err != nil {
		slog.Error("telemetry init failed", "err", err)
		os.Exit(1)
	}

	tracer := otel.Tracer("grpc-auth")

	// ── gRPC server ──────────────────────────────────────────────────────────
	// grpc.ChainUnaryInterceptor executes interceptors in order, outermost first.
	// This is the gRPC equivalent of net/http middleware chaining.
	//
	// Execution order for an incoming CheckToken RPC:
	//   UnaryTraceInterceptor (extract traceparent, open span)
	//     → UnaryLogInterceptor (log method + code after handler returns)
	//       → CheckToken handler
	s := grpc.NewServer(
		grpc.ChainUnaryInterceptor(
			authgrpc.UnaryTraceInterceptor(tracer),
			authgrpc.UnaryLogInterceptor(),
		),
	)

	// Register our AuthService implementation.
	authpb.RegisterAuthServiceServer(s, authgrpc.NewAuthServer(cfg.JWTSecret, tracer))

	// Server reflection lets grpcurl and other tools discover the service
	// schema at runtime without needing the .proto file.
	reflection.Register(s)

	// ── Listen ───────────────────────────────────────────────────────────────
	addr := fmt.Sprintf(":%d", cfg.GRPCAuthPort)
	lis, err := net.Listen("tcp", addr)
	if err != nil {
		slog.Error("grpc-auth: failed to listen", "addr", addr, "err", err)
		os.Exit(1)
	}

	quit := make(chan os.Signal, 1)
	signal.Notify(quit, os.Interrupt, syscall.SIGTERM)

	go func() {
		slog.Info("grpc-auth starting", "addr", addr)
		if err := s.Serve(lis); err != nil {
			slog.Error("grpc-auth: serve error", "err", err)
			os.Exit(1)
		}
	}()

	<-quit
	slog.Info("grpc-auth: shutting down")

	// GracefulStop waits for in-flight RPCs to finish before closing.
	// K8s sends SIGTERM and waits terminationGracePeriodSeconds before SIGKILL.
	stopped := make(chan struct{})
	go func() { s.GracefulStop(); close(stopped) }()
	select {
	case <-stopped:
		slog.Info("grpc-auth: all RPCs finished")
	case <-time.After(10 * time.Second):
		slog.Warn("grpc-auth: grace period exceeded, forcing stop")
		s.Stop()
	}

	// Flush spans to the collector before exit.
	ctx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancel()
	shutdownTel(ctx)
	slog.Info("grpc-auth stopped")
}
