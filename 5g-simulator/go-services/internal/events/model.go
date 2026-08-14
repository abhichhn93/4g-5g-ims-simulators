// Package events defines the event model shared by all Go services and provides
// the Publisher abstraction that decouples producers from the storage backend.
package events

import "time"

// NodeType identifies which 5G NF (Network Function) generated the event.
// Matches the NF types registered in nrf_sim (TS 29.510 §6.1.6.2).
type NodeType string

const (
	NodeAMF NodeType = "AMF"
	NodeUDM NodeType = "UDM"
	NodeSMF NodeType = "SMF"
	NodeUPF NodeType = "UPF"
	NodeNRF NodeType = "NRF"
	NodeGNB NodeType = "GNB"
)

// EventType classifies what happened. These mirror the 5G procedure names from
// TS 23.502 (AMF registration, PDU session establishment, etc.).
type EventType string

const (
	EventNFRegistered    EventType = "NF_REGISTERED"
	EventNFDeregistered  EventType = "NF_DEREGISTERED"
	EventUERegistered    EventType = "UE_REGISTERED"
	EventPDUSession      EventType = "PDU_SESSION_ESTABLISHED"
	EventScenarioStarted EventType = "SCENARIO_STARTED"
	EventScenarioStopped EventType = "SCENARIO_STOPPED"
	EventLogLine         EventType = "LOG_LINE"
)

// SimEvent is the canonical event structure flowing through the Go layer.
// Fields are json-tagged for SSE streaming and REST responses.
// omitempty on SUPI: most NF-level events (NF_REGISTERED, LOG_LINE) have no UE.
type SimEvent struct {
	ID        string    `json:"id"`
	Timestamp time.Time `json:"timestamp"`
	Node      NodeType  `json:"node"`
	EventType EventType `json:"event_type"`
	SUPI      string    `json:"supi,omitempty"` // 5G subscriber identifier, present on UE events
	Detail    string    `json:"detail"`
	Severity  string    `json:"severity"` // "info", "warn", "error"
}

// Publisher is the single abstraction point between event producers and the
// storage/streaming backend.
//
// Why an interface here and nowhere else? Because this is the only seam that
// changes in production: today it's an in-memory BoundedStore; tomorrow it's
// a Kafka producer. Everything else (HTTP handlers, log watchers) depends on
// this interface, not the concrete type.
//
// To add Kafka:
//  1. Create KafkaPublisher struct implementing Publish(SimEvent).
//  2. Wire it in main.go instead of NewBoundedStore.
//  3. The rest of the code is unchanged.
type Publisher interface {
	Publish(e SimEvent)
}
