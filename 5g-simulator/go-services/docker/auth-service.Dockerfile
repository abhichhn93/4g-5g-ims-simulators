# Multi-stage build: stage 1 compiles, stage 2 runs.
# The final image contains only the static binary + minimal libc (alpine).
# Result: ~15 MB image vs ~800 MB if we shipped the Go toolchain.

# ---- Stage 1: build ----
FROM golang:1.23-alpine AS build

WORKDIR /src

# Copy dependency manifests first so Docker caches the module download layer.
# go.sum is required by go mod verify and must be present before go mod download.
COPY go.mod go.sum ./
RUN go mod download

# Copy the rest of the source and build only the auth-service binary.
COPY . .
RUN CGO_ENABLED=0 go build -o /service ./cmd/auth-service/

# ---- Stage 2: run ----
FROM alpine:3.19

# nobody (uid 65534) is a non-root user present in alpine by default.
# Running as non-root is a K8s security best practice (PSA restricted profile).
USER nobody

COPY --from=build /service /service
EXPOSE 8081
ENTRYPOINT ["/service"]
