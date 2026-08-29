package main

import (
	"crypto/subtle"
	"encoding/json"
	"net/http"
	"os"
	"strings"
	"sync/atomic"
)

type signalMetrics struct {
	activeConnections        atomic.Int64
	totalConnections         atomic.Uint64
	rateLimitedHandshakes    atomic.Uint64
	authFailures             atomic.Uint64
	messagesForwarded        atomic.Uint64
	pendingHostAuthQueued    atomic.Uint64
	pendingHostAuthDelivered atomic.Uint64
	pendingHostAuthDropped   atomic.Uint64
}

type signalMetricsSnapshot struct {
	ActiveConnections        int64  `json:"activeConnections"`
	TotalConnections         uint64 `json:"totalConnections"`
	RateLimitedHandshakes    uint64 `json:"rateLimitedHandshakes"`
	AuthFailures             uint64 `json:"authFailures"`
	MessagesForwarded        uint64 `json:"messagesForwarded"`
	PendingHostAuthQueued    uint64 `json:"pendingHostAuthQueued"`
	PendingHostAuthDelivered uint64 `json:"pendingHostAuthDelivered"`
	PendingHostAuthDropped   uint64 `json:"pendingHostAuthDropped"`
}

func (m *signalMetrics) snapshot() signalMetricsSnapshot {
	return signalMetricsSnapshot{
		ActiveConnections:        m.activeConnections.Load(),
		TotalConnections:         m.totalConnections.Load(),
		RateLimitedHandshakes:    m.rateLimitedHandshakes.Load(),
		AuthFailures:             m.authFailures.Load(),
		MessagesForwarded:        m.messagesForwarded.Load(),
		PendingHostAuthQueued:    m.pendingHostAuthQueued.Load(),
		PendingHostAuthDelivered: m.pendingHostAuthDelivered.Load(),
		PendingHostAuthDropped:   m.pendingHostAuthDropped.Load(),
	}
}

func metricsAuthorized(r *http.Request) bool {
	expected := os.Getenv("DESKLINK_METRICS_TOKEN")
	if expected == "" {
		return false
	}
	const prefix = "Bearer "
	authorization := r.Header.Get("Authorization")
	if !strings.HasPrefix(authorization, prefix) {
		return false
	}
	provided := strings.TrimSpace(strings.TrimPrefix(authorization, prefix))
	if len(provided) != len(expected) {
		return false
	}
	return subtle.ConstantTimeCompare([]byte(provided), []byte(expected)) == 1
}

func registerMetricsHandler(m *signalMetrics) {
	http.HandleFunc("/metricsz", func(w http.ResponseWriter, r *http.Request) {
		if os.Getenv("DESKLINK_METRICS_TOKEN") == "" {
			http.NotFound(w, r)
			return
		}
		if !metricsAuthorized(r) {
			w.Header().Set("WWW-Authenticate", `Bearer realm="desklink-metrics"`)
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		_ = json.NewEncoder(w).Encode(m.snapshot())
	})
}
