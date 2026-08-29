package main

import (
	"encoding/json"
	"fmt"
	"testing"
	"time"
)

func TestPendingHostAuthQueueDeduplicatesAndRefreshes(t *testing.T) {
	queue := newPendingHostAuthQueue()
	now := time.Unix(1_700_000_000, 0)

	if !queue.enqueue("host-a", "controller-a", "session-a", json.RawMessage(`{"version":1}`), now) {
		t.Fatal("first auth request should be queued")
	}
	if !queue.enqueue("host-a", "controller-a", "session-a", json.RawMessage(`{"version":2}`), now.Add(time.Second)) {
		t.Fatal("duplicate controller/session should refresh in place")
	}
	if queue.size(now.Add(time.Second)) != 1 {
		t.Fatalf("expected one deduplicated request, got %d", queue.size(now.Add(time.Second)))
	}

	entries := queue.take("host-a", now.Add(2*time.Second))
	if len(entries) != 1 {
		t.Fatalf("expected one pending request, got %d", len(entries))
	}
	if string(entries[0].payload) != `{"version":2}` {
		t.Fatalf("expected refreshed payload, got %s", entries[0].payload)
	}
	if queue.size(now.Add(2*time.Second)) != 0 {
		t.Fatal("take should remove delivered requests")
	}
}

func TestPendingHostAuthQueueExpiresRequests(t *testing.T) {
	queue := newPendingHostAuthQueue()
	now := time.Unix(1_700_000_000, 0)
	if !queue.enqueue("host-a", "controller-a", "session-a", nil, now) {
		t.Fatal("request should be queued")
	}
	if got := queue.take("host-a", now.Add(pendingHostAuthTTL+time.Millisecond)); len(got) != 0 {
		t.Fatalf("expired requests must not be delivered: %+v", got)
	}
	if queue.size(now.Add(pendingHostAuthTTL+time.Millisecond)) != 0 {
		t.Fatal("expired request should be pruned from the queue")
	}
}

func TestPendingHostAuthQueueEnforcesPerTargetLimit(t *testing.T) {
	queue := newPendingHostAuthQueue()
	now := time.Unix(1_700_000_000, 0)

	for i := 0; i < maxPendingHostAuthPerTarget; i++ {
		if !queue.enqueue(
			"host-a",
			fmt.Sprintf("controller-%d", i),
			fmt.Sprintf("session-%d", i),
			nil,
			now,
		) {
			t.Fatalf("request %d should fit within the per-target limit", i)
		}
	}
	if queue.enqueue("host-a", "controller-overflow", "session-overflow", nil, now) {
		t.Fatal("queue must reject requests beyond the per-target limit")
	}
}

func TestPendingHostAuthQueueCopiesPayload(t *testing.T) {
	queue := newPendingHostAuthQueue()
	now := time.Unix(1_700_000_000, 0)
	payload := json.RawMessage(`{"version":1}`)
	if !queue.enqueue("host-a", "controller-a", "session-a", payload, now) {
		t.Fatal("request should be queued")
	}
	payload[0] = '['

	entries := queue.take("host-a", now.Add(time.Second))
	if len(entries) != 1 || string(entries[0].payload) != `{"version":1}` {
		t.Fatalf("queue must own an immutable payload copy, got %q", entries[0].payload)
	}
}

func TestPendingHostAuthRequiresControllerScope(t *testing.T) {
	controller := &peer{id: "controller-a", controller: true}
	host := &peer{id: "host-a", controller: false}

	if !shouldQueuePendingHostAuth(controller, "auth-request") {
		t.Fatal("authenticated controller auth-request should be eligible for offline queueing")
	}
	if shouldQueuePendingHostAuth(host, "auth-request") {
		t.Fatal("host identities must not be able to occupy the offline controller auth queue")
	}
	if shouldQueuePendingHostAuth(controller, "offer") {
		t.Fatal("only auth-request may be queued while the target is offline")
	}
}

func TestHostAuthDispatchGuardSuppressesSameConnectionDuplicate(t *testing.T) {
	guard := newHostAuthDispatchGuard()
	now := time.Unix(1_700_000_000, 0)
	source := &peer{id: "controller-a"}

	if !guard.claim("host-a", source, "session-a", now) {
		t.Fatal("first dispatch should be claimed")
	}
	if guard.claim("host-a", source, "session-a", now.Add(time.Second)) {
		t.Fatal("same connection/session should be suppressed within dedupe TTL")
	}
	if !guard.claim("host-a", source, "session-a", now.Add(hostAuthDispatchDedupeTTL+time.Millisecond)) {
		t.Fatal("dispatch should be allowed again after dedupe TTL")
	}
}

func TestHostAuthDispatchGuardAllowsReconnectedController(t *testing.T) {
	guard := newHostAuthDispatchGuard()
	now := time.Unix(1_700_000_000, 0)
	oldConnection := &peer{id: "controller-a"}
	newConnection := &peer{id: "controller-a"}

	if !guard.claim("host-a", oldConnection, "session-a", now) {
		t.Fatal("first dispatch should be claimed")
	}
	if !guard.claim("host-a", newConnection, "session-a", now.Add(time.Second)) {
		t.Fatal("a new controller connection must not inherit the old connection dedupe window")
	}
}
