// Package grpc implements the gRPC AuthService server.
//
// Why a gRPC auth service?
//
// control-api and event-gateway both need to verify JWT tokens for service-to-
// service calls (e.g. control-api calling event-gateway's internal APIs).
// Currently each service duplicates the auth.Verify() call. A gRPC AuthService
// centralises token verification: one service owns the logic, others call it
// over gRPC. This is the "token introspection" pattern common in microservice auth.
//
// Call flow:
//
//	caller  ──grpc CheckToken──▶  grpc-auth :50051
//	                                │
//	                                └─ auth.Verify(secret, token) → claims
//	                                └─ returns {valid, username, role}
//
// The proto contract lives in proto/auth.proto.
// Generated types are in internal/grpc/authpb/ (run scripts/generate_proto.sh).
package grpc

import (
	"context"
	"log/slog"

	"go.opentelemetry.io/otel"
	otelcodes "go.opentelemetry.io/otel/codes"
	"go.opentelemetry.io/otel/propagation"
	"go.opentelemetry.io/otel/trace"
	googlegrpc "google.golang.org/grpc"
	"google.golang.org/grpc/codes"
	"google.golang.org/grpc/metadata"
	"google.golang.org/grpc/status"

	"go-services/internal/auth"
	"go-services/internal/grpc/authpb"
)

// AuthServer implements authpb.AuthServiceServer.
// Embedding UnimplementedAuthServiceServer satisfies the interface for any
// future RPC methods added to the proto — the compiler will remind you to
// implement new methods but won't break existing deployments.
type AuthServer struct {
	authpb.UnimplementedAuthServiceServer
	jwtSecret string
	tracer    trace.Tracer
}

// NewAuthServer returns a ready-to-register gRPC server. The jwtSecret must
// match the one used by auth-service to issue tokens.
func NewAuthServer(jwtSecret string, tracer trace.Tracer) *AuthServer {
	return &AuthServer{jwtSecret: jwtSecret, tracer: tracer}
}

// CheckToken validates a JWT and returns the caller's identity.
// Called by other microservices that receive a user token and want to know
// who it belongs to without re-implementing JWT parsing themselves.
//
// gRPC error codes mirror HTTP:
//   - codes.InvalidArgument  → bad input (no token)
//   - codes.Unauthenticated  → token invalid / expired
//   - codes.Internal         → unexpected server error
func (s *AuthServer) CheckToken(ctx context.Context, req *authpb.AuthCheckRequest) (*authpb.AuthCheckResponse, error) {
	// Start a child span. The parent span was created by UnaryTraceInterceptor,
	// so this shows as a nested span in Jaeger:
	//
	//   /auth.AuthService/CheckToken   ← interceptor span (SpanKindServer)
	//     └─ auth.verify               ← this span
	ctx, span := s.tracer.Start(ctx, "auth.verify")
	defer span.End()

	if req.Token == "" {
		span.SetStatus(otelcodes.Error, "empty token")
		return nil, status.Error(codes.InvalidArgument, "token is required")
	}

	claims, err := auth.Verify(s.jwtSecret, req.Token)
	if err != nil {
		// RecordError attaches the error to the span (visible in Jaeger detail view).
		span.RecordError(err)
		span.SetStatus(otelcodes.Error, "token verification failed")
		slog.WarnContext(ctx, "CheckToken: invalid token", "err", err)
		// Return valid=false in the response body (not a gRPC error) so the
		// caller can distinguish "bad token" from "service unavailable".
		return &authpb.AuthCheckResponse{
			Valid:  false,
			Reason: err.Error(),
		}, nil
	}

	span.SetStatus(otelcodes.Ok, "")
	return &authpb.AuthCheckResponse{
		Valid:    true,
		Username: claims.Username,
		Role:     string(claims.Role),
	}, nil
}

// ─── INTERCEPTORS ───────────────────────────────────────────────────────────
//
// An interceptor is gRPC's equivalent of HTTP middleware. It wraps every RPC
// call — you get to run code before and after the handler, just like net/http
// middleware wraps ServeHTTP. Multiple interceptors are chained via
// grpc.ChainUnaryInterceptor — they execute in order (outermost first).

// UnaryTraceInterceptor creates a span for every incoming gRPC call.
// It extracts the W3C traceparent from the gRPC metadata (injected by the
// caller's otelhttp middleware) so the gRPC span is a CHILD of the caller's
// HTTP span — same trace ID, waterfall view in Jaeger.
func UnaryTraceInterceptor(tracer trace.Tracer) googlegrpc.UnaryServerInterceptor {
	return func(
		ctx context.Context,
		req any,
		info *googlegrpc.UnaryServerInfo,
		handler googlegrpc.UnaryHandler,
	) (any, error) {
		// Extract traceparent from incoming gRPC metadata headers.
		// gRPC metadata is the gRPC equivalent of HTTP headers — key=value
		// pairs sent alongside the RPC request on the same connection.
		if md, ok := metadata.FromIncomingContext(ctx); ok {
			ctx = otel.GetTextMapPropagator().Extract(ctx, metadataCarrier(md))
		}

		ctx, span := tracer.Start(ctx, info.FullMethod,
			trace.WithSpanKind(trace.SpanKindServer),
		)
		defer span.End()

		resp, err := handler(ctx, req)
		if err != nil {
			span.RecordError(err)
			span.SetStatus(otelcodes.Error, err.Error())
		} else {
			span.SetStatus(otelcodes.Ok, "")
		}
		return resp, err
	}
}

// UnaryLogInterceptor logs every RPC call with method name and outcome.
// Chained after UnaryTraceInterceptor via grpc.ChainUnaryInterceptor.
func UnaryLogInterceptor() googlegrpc.UnaryServerInterceptor {
	return func(
		ctx context.Context,
		req any,
		info *googlegrpc.UnaryServerInfo,
		handler googlegrpc.UnaryHandler,
	) (any, error) {
		resp, err := handler(ctx, req)
		code := codes.OK
		if err != nil {
			code = status.Code(err)
		}
		slog.InfoContext(ctx, "grpc call", "method", info.FullMethod, "code", code)
		return resp, err
	}
}

// ─── TRACE CONTEXT EXTRACTION ───────────────────────────────────────────────
//
// W3C TraceContext propagation over gRPC metadata.
// gRPC metadata uses lowercase ASCII keys, same as HTTP headers.
// The traceparent value looks like:
//
//	"00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"
//	      │                               │                 └─ flags (01=sampled)
//	      │                               └─ parent-id (64-bit: calling span)
//	      └─ trace-id (128-bit: the same across ALL services in this request)

// metadataCarrier adapts gRPC metadata.MD to propagation.TextMapCarrier so
// the W3C TraceContext propagator can read/write traceparent from gRPC metadata.
type metadataCarrier metadata.MD

func (mc metadataCarrier) Get(key string) string {
	vals := metadata.MD(mc).Get(key)
	if len(vals) == 0 {
		return ""
	}
	return vals[0]
}

func (mc metadataCarrier) Set(key, val string) {
	metadata.MD(mc).Set(key, val)
}

func (mc metadataCarrier) Keys() []string {
	keys := make([]string, 0, len(mc))
	for k := range mc {
		keys = append(keys, k)
	}
	return keys
}

// ensure metadataCarrier implements TextMapCarrier at compile time
var _ propagation.TextMapCarrier = metadataCarrier{}
