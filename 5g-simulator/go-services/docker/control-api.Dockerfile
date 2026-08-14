# Multi-stage build for control-api.
# Note: control-api uses os/exec to launch C++ binaries. In a container this
# means the C++ binaries must be in the same image or on a shared volume.
# For Docker-compose mode, mount the host build/ directory at /app/build.
# For K8s: replace exec.Command with client-go calls to scale Deployments.

FROM golang:1.23-alpine AS build

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o /service ./cmd/control-api/

FROM alpine:3.19

USER nobody
COPY --from=build /service /service
EXPOSE 8082
ENTRYPOINT ["/service"]
