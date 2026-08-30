package main

import (
	"testing"
	"time"
)

func TestValidDeviceID(t *testing.T) {
	t.Parallel()

	valid := []string{
		"office-pc",
		"win_DESKTOP.01",
		"A1",
	}
	for _, id := range valid {
		if !validDeviceID(id) {
			t.Fatalf("expected %q to be valid", id)
		}
	}

	invalid := []string{
		"",
		"space id",
		"slash/id",
		"中文设备",
	}
	for _, id := range invalid {
		if validDeviceID(id) {
			t.Fatalf("expected %q to be invalid", id)
		}
	}

	tooLong := make([]byte, 129)
	for i := range tooLong {
		tooLong[i] = 'a'
	}
	if validDeviceID(string(tooLong)) {
		t.Fatal("expected a 129-byte device id to be invalid")
	}
}

func TestSessionAndSignalValidation(t *testing.T) {
	t.Parallel()

	if !validSessionID("session-123") {
		t.Fatal("expected normal session id to be valid")
	}
	if validSessionID("") {
		t.Fatal("expected empty session id to be invalid")
	}

	for _, messageType := range []string{"offer", "answer", "ice", "session-request", "session-accept"} {
		if !allowedSignalType(messageType) {
			t.Fatalf("expected %q to be allowed", messageType)
		}
	}
	for _, messageType := range []string{"control", "telemetry", "admin", ""} {
		if allowedSignalType(messageType) {
			t.Fatalf("expected %q to be rejected", messageType)
		}
	}
}

func TestPeerOfflineSignalIsTargetScoped(t *testing.T) {
	t.Parallel()

	const target = "windows-host-1"
	plain := peerOfflineSignal(target, false)
	if plain["type"] != "peer-offline" {
		t.Fatalf("unexpected peer-offline signal type: %#v", plain["type"])
	}
	if plain["target"] != target {
		t.Fatalf("peer-offline signal lost target scope: %#v", plain["target"])
	}
	if _, exists := plain["payload"]; exists {
		t.Fatal("plain peer-offline signal unexpectedly included a payload")
	}

	queued := peerOfflineSignal(target, true)
	if queued["target"] != target {
		t.Fatalf("queued peer-offline signal lost target scope: %#v", queued["target"])
	}
	payload, ok := queued["payload"].(map[string]any)
	if !ok {
		t.Fatalf("queued peer-offline payload has unexpected type: %T", queued["payload"])
	}
	if payload["authQueued"] != true {
		t.Fatalf("queued peer-offline signal lost authQueued flag: %#v", payload["authQueued"])
	}
	if payload["expiresInMs"] != pendingHostAuthTTL.Milliseconds() || pendingHostAuthTTL <= 0 {
		t.Fatalf("queued peer-offline signal has invalid wait TTL: %#v", payload["expiresInMs"])
	}
}

func TestSignalRateLimiter(t *testing.T) {
	p := newPeer("test-device", nil)

	for i := 0; i < int(signalBurst); i++ {
		if !p.allowSignal() {
			t.Fatalf("burst token %d was rejected", i)
		}
	}
	if p.allowSignal() {
		t.Fatal("expected request after burst exhaustion to be rejected")
	}

	p.rateMu.Lock()
	p.tokens = 0
	p.lastRefill = time.Now().Add(-time.Second)
	p.rateMu.Unlock()

	if !p.allowSignal() {
		t.Fatal("expected tokens to refill over time")
	}
}
