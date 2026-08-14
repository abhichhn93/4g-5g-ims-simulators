# Multi-stage build for event-gateway.
# event-gateway tails C++ log files from SimLogDir. In Docker-compose mode,
# mount the host /tmp (or wherever C++ NFs write) into the container at /tmp.
# In K8s: mount a shared emptyDir volume between C++ pods and this service,
# or replace the log watcher with a Kafka consumer.

FROM golang:1.23-alpine AS build

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o /service ./cmd/event-gateway/

FROM alpine:3.19

USER nobody
COPY --from=build /service /service
EXPOSE 8083
ENTRYPOINT ["/service"]
