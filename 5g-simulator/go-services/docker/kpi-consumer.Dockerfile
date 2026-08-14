# Multi-stage build for kpi-consumer, same pattern as the other Go services'
# Dockerfiles in this directory.

FROM golang:1.23-alpine AS build

WORKDIR /src
COPY go.mod go.sum ./
RUN go mod download
COPY . .
RUN CGO_ENABLED=0 go build -o /service ./cmd/kpi-consumer/

FROM alpine:3.19

USER nobody
COPY --from=build /service /service
EXPOSE 8084
ENTRYPOINT ["/service"]
