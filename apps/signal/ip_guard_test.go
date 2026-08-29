package main

import (
	"net/http/httptest"
	"os"
	"testing"
	"time"
)

func TestIPGuardLimitsConcurrentConnections(t *testing.T) {
	guard := newIPGuard()
	now := time.Unix(1_700_000_000, 0)
	ip := "203.0.113.10"

	for i := 0; i < defaultMaxConnectionsPerIP; i++ {
		if !guard.reserve(ip, now) {
			t.Fatalf("connection %d should have been admitted", i+1)
		}
	}
	if guard.reserve(ip, now) {
		t.Fatal("connection above per-IP concurrent limit should be rejected")
	}

	guard.release(ip, now)
	if !guard.reserve(ip, now.Add(time.Second)) {
		t.Fatal("released connection slot should become available")
	}
}

func TestIPGuardBlocksRepeatedAuthenticationFailures(t *testing.T) {
	guard := newIPGuard()
	now := time.Unix(1_700_000_000, 0)
	ip := "198.51.100.20"

	for i := 0; i < authFailureBlockThreshold; i++ {
		guard.authFailed(ip, now)
	}
	if guard.reserve(ip, now.Add(time.Second)) {
		t.Fatal("IP should be temporarily blocked after repeated auth failures")
	}
	if !guard.reserve(ip, now.Add(authFailureBlockDuration+time.Second)) {
		t.Fatal("IP should be admitted again after the temporary block expires")
	}
}

func TestIPGuardAuthSuccessClearsFailureState(t *testing.T) {
	guard := newIPGuard()
	now := time.Unix(1_700_000_000, 0)
	ip := "192.0.2.30"

	for i := 0; i < authFailureBlockThreshold-1; i++ {
		guard.authFailed(ip, now)
	}
	guard.authSucceeded(ip, now)
	guard.authFailed(ip, now)

	if !guard.reserve(ip, now.Add(time.Second)) {
		t.Fatal("successful authentication should reset accumulated failure state")
	}
}

func TestRequestClientIPTrustsProxyHeadersOnlyWhenEnabled(t *testing.T) {
	previous := os.Getenv("DESKLINK_TRUST_PROXY_HEADERS")
	t.Cleanup(func() { _ = os.Setenv("DESKLINK_TRUST_PROXY_HEADERS", previous) })

	req := httptest.NewRequest("GET", "http://desklink.test/ws", nil)
	req.RemoteAddr = "10.0.0.5:54321"
	req.Header.Set("X-Forwarded-For", "203.0.113.7, 10.0.0.1")

	_ = os.Unsetenv("DESKLINK_TRUST_PROXY_HEADERS")
	if got := requestClientIP(req); got != "10.0.0.5" {
		t.Fatalf("untrusted proxy header must be ignored, got %q", got)
	}

	_ = os.Setenv("DESKLINK_TRUST_PROXY_HEADERS", "1")
	if got := requestClientIP(req); got != "203.0.113.7" {
		t.Fatalf("trusted proxy header should supply client IP, got %q", got)
	}
}
