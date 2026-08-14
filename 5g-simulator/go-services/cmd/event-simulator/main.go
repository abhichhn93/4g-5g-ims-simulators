// event-simulator: publishes synthetic SimEvents straight to Kafka, one per
// second, without needing the C++ NF binaries or NRF running.
//
// Why this exists: event-gateway only produces events by tailing real C++ NF
// log files or polling a real NRF — great for the full stack, useless for
// quickly exercising the Kafka pipeline (kpi-consumer's windowing, retry/DLQ,
// idempotency) in isolation. This is that quick path: point it at a broker
// and watch kpi-consumer print KPI lines within a couple of windows.
package main

import (
	"fmt"
	"log/slog"
	"math/rand"
	"os"
	"time"

	"go-services/internal/config"
	"go-services/internal/events"
)

var nodes = []events.NodeType{events.NodeAMF, events.NodeUDM, events.NodeSMF, events.NodeUPF, events.NodeGNB}

func main() {
	cfg := config.Load()
	if cfg.KafkaBrokers == "" {
		slog.Error("KAFKA_BROKERS is not set — nothing to publish to")
		os.Exit(1)
	}
	pub := events.NewKafkaPublisher(cfg.KafkaBrokers, cfg.KafkaTopic)
	defer pub.Close()

	fmt.Printf("event-simulator: publishing to topic %q on %s (Ctrl+C to stop)\n", cfg.KafkaTopic, cfg.KafkaBrokers)

	var n int
	for range time.Tick(1 * time.Second) {
		n++
		node := nodes[rand.Intn(len(nodes))]
		sev := "info"
		if rand.Intn(10) == 0 { // ~10% error rate, so the KPI line has something to show
			sev = "error"
		}
		e := events.SimEvent{
			ID:        fmt.Sprintf("sim-%d", n),
			Timestamp: time.Now(),
			Node:      node,
			EventType: events.EventUERegistered,
			SUPI:      fmt.Sprintf("imsi-%03d", rand.Intn(20)),
			Detail:    "synthetic event from event-simulator",
			Severity:  sev,
		}
		pub.Publish(e)
		fmt.Printf("sent  id=%-8s node=%-3s supi=%-10s severity=%s\n", e.ID, e.Node, e.SUPI, e.Severity)
	}
}
