package events

import "sync"

// BoundedStore is a thread-safe circular buffer of SimEvents with fan-out to
// SSE subscribers.
//
// This is equivalent to Kafka consumer groups at tiny scale. The bounded buffer
// handles burst; subscribers drain it via channels. In production, replace
// Publish() with a Kafka producer and Subscribe/Unsubscribe with a Kafka
// consumer group. The Publisher interface in model.go is the only code change
// required — all callers stay the same.
type BoundedStore struct {
	mu       sync.RWMutex
	buf      []SimEvent
	capacity int
	writeIdx int // next write position (wraps at capacity)
	total    int // total events ever written (to distinguish empty vs full)

	// fan-out: each subscriber gets its own buffered channel so a slow SSE
	// client doesn't block event publication. Use a separate mutex because
	// subscriber map operations happen inside Publish() which already holds mu.
	muSubs  sync.Mutex
	subs    map[int]chan SimEvent
	nextSub int
}

// NewBoundedStore creates a store that retains at most capacity events.
// Capacity 1000 is enough for a simulator session; tune upward for longer runs.
func NewBoundedStore(capacity int) *BoundedStore {
	return &BoundedStore{
		buf:      make([]SimEvent, capacity),
		capacity: capacity,
		subs:     make(map[int]chan SimEvent),
	}
}

// Publish stores the event in the circular buffer and fans it out to all active
// SSE subscribers. Implements the Publisher interface.
//
// The circular buffer overwrites the oldest event when full — appropriate for a
// live-view tool. If you need durable history, swap for Kafka here.
func (s *BoundedStore) Publish(e SimEvent) {
	s.mu.Lock()
	s.buf[s.writeIdx] = e
	s.writeIdx = (s.writeIdx + 1) % s.capacity
	s.total++
	s.mu.Unlock()

	// Fan-out to subscribers. Non-blocking send: if the subscriber's channel
	// buffer is full (slow consumer), we drop the event for that subscriber
	// rather than blocking all other subscribers.
	s.muSubs.Lock()
	for _, ch := range s.subs {
		select {
		case ch <- e:
		default:
			// slow subscriber: event dropped for this consumer only
		}
	}
	s.muSubs.Unlock()
}

// Recent returns the last n events, oldest first (chronological order).
// Returns fewer than n if the store hasn't seen that many events yet.
func (s *BoundedStore) Recent(n int) []SimEvent {
	s.mu.RLock()
	defer s.mu.RUnlock()

	count := s.total
	if count > s.capacity {
		count = s.capacity
	}
	if n > count {
		n = count
	}
	if n == 0 {
		return nil
	}

	result := make([]SimEvent, n)
	// Start index for the oldest of the last n events.
	// writeIdx points to the next write slot (= oldest slot when buffer is full).
	startIdx := (s.writeIdx - n + s.capacity*2) % s.capacity
	for i := 0; i < n; i++ {
		result[i] = s.buf[(startIdx+i)%s.capacity]
	}
	return result
}

// Subscribe returns a subscriber ID and a read-only channel that receives new
// events as they are published. The channel is buffered (64 events) to tolerate
// momentary slow consumers. Caller MUST call Unsubscribe when done.
func (s *BoundedStore) Subscribe() (int, <-chan SimEvent) {
	ch := make(chan SimEvent, 64)
	s.muSubs.Lock()
	id := s.nextSub
	s.nextSub++
	s.subs[id] = ch
	s.muSubs.Unlock()
	return id, ch
}

// Unsubscribe removes the subscriber and closes its channel. Safe to call
// multiple times (idempotent after first call).
func (s *BoundedStore) Unsubscribe(id int) {
	s.muSubs.Lock()
	if ch, ok := s.subs[id]; ok {
		delete(s.subs, id)
		close(ch)
	}
	s.muSubs.Unlock()
}
