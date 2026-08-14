#!/usr/bin/env bash
# generate_proto.sh — Regenerate Go code from proto/auth.proto
#
# Run this whenever proto/auth.proto changes.
# Output goes to internal/grpc/authpb/ (checked in — no codegen needed to build).
#
# Requires:
#   brew install protobuf
#   go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
#   go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$SCRIPT_DIR/.."

export PATH="$(go env GOPATH)/bin:$PATH"

mkdir -p "$ROOT/internal/grpc/authpb"

protoc \
  --proto_path="$ROOT" \
  --go_out="$ROOT/internal/grpc/authpb" \
  --go_opt=paths=import \
  --go_opt=Mproto/auth.proto=go-services/internal/grpc/authpb \
  --go-grpc_out="$ROOT/internal/grpc/authpb" \
  --go-grpc_opt=paths=import \
  --go-grpc_opt=Mproto/auth.proto=go-services/internal/grpc/authpb \
  "$ROOT/proto/auth.proto"

echo "Generated files in internal/grpc/authpb/:"
ls "$ROOT/internal/grpc/authpb/"
