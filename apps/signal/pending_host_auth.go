package main

import (
	"encoding/json"
	"log"
	"sync"
	"time"
)

const (
	pendingHostAuthTTL           = 30 * time.Second
	maxPendingHostAuthGlobal     = 512
	maxPendingHostAuthPerTarget  = 32
)

type pendingHostAuthSignal struct {
	from      string
	session   string
	payload   json.RawMessage
	expiresAt time.Time
}

type pendingHostAuthQueue struct {
	mu       sync.Mutex
	byTarget map[string]map[string]pendingHostAuthSignal
	total    int
}

func newPendingHostAuthQueue() *pendingHostAuthQueue {
	return &pendingHostAuthQueue{
		byTarget: make(map[string]map[string]pendingHostAuthSignal),
	}
}

func pendingHostAuthKey(from, session string) string {
	return from + "\x00" + session
}

func (q *pendingHostAuthQueue) pruneExpiredLocked(now time.Time) {
	for target, entries := range q.byTarget {
		for key, entry := range entries {
			if entry.expiresAt.After(now) {
				continue
			}
			delete(entries, key)
			q.total--
		}
		if len(entries) == 0 {
			delete(q.byTarget, target)
		}
	}
}

func (q *pendingHostAuthQueue) enqueue(
	target string,
	from string,
	session string,
	payload json.RawMessage,
	now time.Time,
) bool {
	if target == "" || from == "" || session == "" {
		return false
	}

	q.mu.Lock()
	defer q.mu.Unlock()
	q.pruneExpiredLocked(now)

	entries := q.byTarget[target]
	if entries == nil {
		entries = make(map[string]pendingHostAuthSignal)
		q.byTarget[target] = entries
	}

	key := pendingHostAuthKey(from, session)
	if _, exists := entries[key]; !exists {
		if len(entries) >= maxPendingHostAuthPerTarget || q.total >= maxPendingHostAuthGlobal {
			return false
		}
		q.total++
	}

	entries[key] = pendingHostAuthSignal{
		from:      from,
		session:   session,
		payload:   append(json.RawMessage(nil), payload...),
		expiresAt: now.Add(pendingHostAuthTTL),
	}
	return true
}

func (q *pendingHostAuthQueue) take(target string, now time.Time) []pendingHostAuthSignal {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.pruneExpiredLocked(now)

	entries := q.byTarget[target]
	if len(entries) == 0 {
		return nil
	}

	delete(q.byTarget, target)
	q.total -= len(entries)
	result := make([]pendingHostAuthSignal, 0, len(entries))
	for _, entry := range entries {
		result = append(result, entry)
	}
	return result
}

func (q *pendingHostAuthQueue) size(now time.Time) int {
	q.mu.Lock()
	defer q.mu.Unlock()
	q.pruneExpiredLocked(now)
	return q.total
}

var pendingHostAuthRequests = newPendingHostAuthQueue()

func flushPendingHostAuthRequests(h *hub, target *peer, metrics *signalMetrics) {
	requests := pendingHostAuthRequests.take(target.id, time.Now())
	for index, request := range requests {
		source := h.get(request.from)
		if source == nil || !source.canSignalTarget(target.id) {
			metrics.pendingHostAuthDropped.Add(1)
			continue
		}

		revoked, err := deviceRevoked(target.id)
		if err != nil || revoked {
			metrics.pendingHostAuthDropped.Add(1)
			continue
		}

		forward := map[string]any{
			"type":    "auth-request",
			"from":    request.from,
			"session": request.session,
			"payload": request.payload,
		}
		if err := target.write(forward); err != nil {
			log.Printf("flush pending auth %s -> %s: %v", request.from, target.id, err)
			for _, retry := range requests[index:] {
				if !pendingHostAuthRequests.enqueue(
					target.id,
					retry.from,
					retry.session,
					retry.payload,
					time.Now(),
				) {
					metrics.pendingHostAuthDropped.Add(1)
				}
			}
			return
		}

		metrics.messagesForwarded.Add(1)
		metrics.pendingHostAuthDelivered.Add(1)
	}
}
