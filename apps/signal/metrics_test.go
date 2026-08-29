package main

import (
	"net/http/httptest"
	"os"
	"testing"
)

func TestMetricsAuthorizedRequiresConfiguredBearerToken(t *testing.T) {
	previous := os.Getenv("DESKLINK_METRICS_TOKEN")
	t.Cleanup(func() { _ = os.Setenv("DESKLINK_METRICS_TOKEN", previous) })

	req := httptest.NewRequest("GET", "http://desklink.test/metricsz", nil)
	_ = os.Unsetenv("DESKLINK_METRICS_TOKEN")
	if metricsAuthorized(req) {
		t.Fatal("metrics must stay disabled without a configured token")
	}

	_ = os.Setenv("DESKLINK_METRICS_TOKEN", "metrics-secret")
	req.Header.Set("Authorization", "Bearer wrong-secret")
	if metricsAuthorized(req) {
		t.Fatal("wrong bearer token must be rejected")
	}

	req.Header.Set("Authorization", "Bearer metrics-secret")
	if !metricsAuthorized(req) {
		t.Fatal("correct bearer token should be accepted")
	}
}

func TestSignalMetricsSnapshot(t *testing.T) {
	var metrics signalMetrics
	metrics.activeConnections.Store(2)
	metrics.totalConnections.Store(9)
	metrics.rateLimitedHandshakes.Store(3)
	metrics.authFailures.Store(4)
	metrics.messagesForwarded.Store(17)

	snapshot := metrics.snapshot()
	if snapshot.ActiveConnections != 2 || snapshot.TotalConnections != 9 ||
		snapshot.RateLimitedHandshakes != 3 || snapshot.AuthFailures != 4 ||
		snapshot.MessagesForwarded != 17 {
		t.Fatalf("unexpected snapshot: %+v", snapshot)
	}
}
