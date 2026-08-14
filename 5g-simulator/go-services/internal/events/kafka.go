package events

import (
	"context"
	"encoding/json"
	"log/slog"
	"strings"
	"time"

	"github.com/segmentio/kafka-go"
)

// KafkaPublisher implements the Publisher interface (model.go) on top of a real
// Kafka broker, using the segmentio/kafka-go client. This is the "production"
// half of the seam BoundedStore's doc comment describes: everything upstream
// of Publish() — logWatcher, nrfPoller, HTTP handlers — is unaware this exists.
type KafkaPublisher struct {
	writer *kafka.Writer
}

// NewKafkaPublisher connects to the given brokers (comma-separated host:port)
// and returns a publisher for topic. Connection is lazy — kafka-go dials on
// the first WriteMessages call, so this never blocks or fails at startup even
// if the broker isn't up yet (useful for docker-compose startup ordering).
func NewKafkaPublisher(brokers, topic string) *KafkaPublisher {
	return &KafkaPublisher{
		writer: &kafka.Writer{
			Addr:         kafka.TCP(strings.Split(brokers, ",")...),
			Topic:        topic,
			Balancer:     &kafka.Hash{}, // hash the key -> same key always lands on the same partition
			RequiredAcks: kafka.RequireOne,
			BatchTimeout: 100 * time.Millisecond, // don't hold small event bursts back
		},
	}
}

// Publish sends the event to Kafka, keyed by SUPI.
//
// Why key by SUPI: Kafka only guarantees ordering *within a partition*, not
// across the whole topic. If two events for the same subscriber (e.g.
// UE_REGISTERED then PDU_SESSION_ESTABLISHED) landed on different partitions,
// a consumer could see them out of order or process them on different
// goroutines concurrently. Hashing on SUPI pins every event for one UE to the
// same partition, so per-subscriber ordering is preserved even though the
// topic as a whole is processed in parallel across partitions. NF-level
// events with no SUPI (NF_REGISTERED, LOG_LINE) key on Node instead — order
// only matters per-NF, not globally.
//
// Publish never blocks the caller on a slow/down broker for long: kafka-go
// retries internally, but if it ultimately fails we log and drop rather than
// propagate an error, because Publisher.Publish has no error return (it's
// called from hot paths like logWatcher that must never stall on I/O).
func (k *KafkaPublisher) Publish(e SimEvent) {
	key := e.SUPI
	if key == "" {
		key = string(e.Node)
	}
	data, err := json.Marshal(e)
	if err != nil {
		slog.Error("kafka publish: marshal failed", "err", err, "event_id", e.ID)
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), 3*time.Second)
	defer cancel()
	if err := k.writer.WriteMessages(ctx, kafka.Message{Key: []byte(key), Value: data}); err != nil {
		slog.Error("kafka publish failed", "err", err, "event_id", e.ID)
	}
}

// Close flushes and closes the underlying connection. Call on shutdown.
func (k *KafkaPublisher) Close() error {
	return k.writer.Close()
}

// FanOutPublisher publishes to two backends: a local BoundedStore (so
// /events and /events/stream keep working with zero latency, unaffected by
// Kafka being slow or down) and Kafka (for durable, replayable, cross-service
// consumption — e.g. kpi-consumer). This mirrors a common real-world pattern:
// keep a fast local cache for the UI, ship the same data to a durable log for
// everything downstream.
type FanOutPublisher struct {
	Local *BoundedStore
	Kafka *KafkaPublisher
}

func (f *FanOutPublisher) Publish(e SimEvent) {
	f.Local.Publish(e)
	f.Kafka.Publish(e)
}
