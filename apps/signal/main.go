package main

import (
	"context"
	"encoding/json"
	"log"
	"math"
	"net/http"
	"os"
	osSignal "os/signal"
	"strings"
	"sync"
	"syscall"
	"time"

	"github.com/gorilla/websocket"
)

const (
	pongWait        = 75 * time.Second
	pingPeriod      = 30 * time.Second
	writeWait       = 8 * time.Second
	maxMessageBytes = 256 << 10
	signalRate       = 30.0
	signalBurst      = 60.0
)

type envelope struct {
	Type     string          `json:"type"`
	DeviceID string          `json:"deviceId,omitempty"`
	Session  string          `json:"session,omitempty"`
	Target   string          `json:"target,omitempty"`
	Payload  json.RawMessage `json:"payload,omitempty"`
}

type peer struct {
	id            string
	conn          *websocket.Conn
	allowedTarget string
	controller    bool
	mu            sync.Mutex

	rateMu     sync.Mutex
	tokens     float64
	lastRefill time.Time
}

type hub struct {
	mu    sync.RWMutex
	peers map[string]*peer
}

func newHub() *hub { return &hub{peers: make(map[string]*peer)} }

func newPeer(id string, conn *websocket.Conn, scopes ...signalRegistrationScope) *peer {
	var scope signalRegistrationScope
	if len(scopes) > 0 {
		scope = scopes[0]
	}
	return &peer{
		id:            id,
		conn:          conn,
		allowedTarget: scope.AllowedTarget,
		controller:    scope.Controller,
		tokens:         signalBurst,
		lastRefill:    time.Now(),
	}
}

func (h *hub) put(p *peer) {
	h.mu.Lock()
	old := h.peers[p.id]
	h.peers[p.id] = p
	h.mu.Unlock()

	// Do not close a replaced socket while holding the hub lock. The old
	// connection's teardown path may concurrently call remove(), and keeping I/O
	// outside the registry critical section prevents reconnect churn from
	// stalling unrelated peer lookups/registrations.
	if old != nil && old != p && old.conn != nil {
		_ = old.conn.Close()
	}
}

func (h *hub) remove(id string, p *peer) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if h.peers[id] == p {
		delete(h.peers, id)
	}
}

func (h *hub) get(id string) *peer {
	h.mu.RLock()
	defer h.mu.RUnlock()
	return h.peers[id]
}

func (h *hub) closeAll(reason string) {
	h.mu.Lock()
	peers := make([]*peer, 0, len(h.peers))
	for _, p := range h.peers {
		peers = append(peers, p)
	}
	h.peers = make(map[string]*peer)
	h.mu.Unlock()

	for _, p := range peers {
		p.closeWithReason(reason)
	}
}

func (p *peer) allowSignal() bool {
	p.rateMu.Lock()
	defer p.rateMu.Unlock()

	now := time.Now()
	elapsed := now.Sub(p.lastRefill).Seconds()
	p.lastRefill = now
	p.tokens = math.Min(signalBurst, p.tokens+elapsed*signalRate)
	if p.tokens < 1 {
		return false
	}
	p.tokens--
	return true
}

func (p *peer) canSignalTarget(target string) bool {
	return p.allowedTarget == "" || target == p.allowedTarget
}

func (p *peer) write(v any) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	_ = p.conn.SetWriteDeadline(time.Now().Add(writeWait))
	return p.conn.WriteJSON(v)
}

func (p *peer) ping() error {
	p.mu.Lock()
	defer p.mu.Unlock()
	return p.conn.WriteControl(websocket.PingMessage, nil, time.Now().Add(writeWait))
}

func (p *peer) closeWithReason(reason string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	deadline := time.Now().Add(writeWait)
	_ = p.conn.WriteControl(
		websocket.CloseMessage,
		websocket.FormatCloseMessage(websocket.CloseGoingAway, reason),
		deadline,
	)
	_ = p.conn.Close()
}

func validDeviceID(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, r := range id {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') || (r >= '0' && r <= '9') || r == '-' || r == '_' || r == '.' {
			continue
		}
		return false
	}
	return true
}

func validSessionID(id string) bool {
	return id != "" && len(id) <= 128
}

func allowedSignalType(messageType string) bool {
	switch messageType {
	case "offer", "answer", "ice",
		"auth-request", "auth-challenge", "auth-proof", "auth-accepted", "auth-rejected",
		"session-request", "session-accept":
		return true
	default:
		return false
	}
}

func originAllowed(origin string) bool {
	if os.Getenv("DESKLINK_ALLOW_ANY_ORIGIN") == "1" || origin == "" {
		return true
	}
	for _, allowed := range strings.Split(os.Getenv("DESKLINK_ALLOWED_ORIGINS"), ",") {
		if strings.TrimSpace(allowed) == origin {
			return true
		}
	}
	return false
}

var upgrader = websocket.Upgrader{
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
	Subprotocols:    []string{"desklink-v1"},
	CheckOrigin: func(r *http.Request) bool {
		return originAllowed(r.Header.Get("Origin"))
	},
}

func main() {
	h := newHub()
	guard := newIPGuard()
	var metrics signalMetrics
	registerMetricsHandler(&metrics)
	http.HandleFunc("/api/v1/signal-token", signalTokenHandler())
	http.HandleFunc("/api/v1/controller-session", controllerSessionHandler(guard))
	http.HandleFunc("/api/v1/turn-credentials", turnCredentialHandler())

	http.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"ok":true}`))
	})

	http.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		clientIP := requestClientIP(r)
		if !guard.reserve(clientIP, time.Now()) {
			metrics.rateLimitedHandshakes.Add(1)
			w.Header().Set("Retry-After", "45")
			http.Error(w, "too many connection attempts", http.StatusTooManyRequests)
			return
		}
		defer func() {
			guard.release(clientIP, time.Now())
		}()

		id := r.URL.Query().Get("deviceId")
		if !validDeviceID(id) {
			http.Error(w, "valid deviceId is required", http.StatusBadRequest)
			return
		}

		scope, authorized := signalRegistrationScopeForRequest(r, id)
		if !authorized {
			guard.authFailed(clientIP, time.Now())
			metrics.authFailures.Add(1)
			http.Error(w, "unauthorized device registration", http.StatusUnauthorized)
			return
		}

		// Device revocation applies directly to host identities. Controller peers
		// are ephemeral browser IDs; their authorization is instead bound to the
		// target device encoded in the controller session token.
		revocationID := id
		if scope.Controller {
			revocationID = scope.AllowedTarget
		}
		revoked, revocationErr := deviceRevoked(revocationID)
		if revocationErr != nil {
			http.Error(w, "device revocation state unavailable", http.StatusServiceUnavailable)
			return
		}
		if revoked {
			metrics.authFailures.Add(1)
			http.Error(w, "device revoked", http.StatusForbidden)
			return
		}
		guard.authSucceeded(clientIP, time.Now())

		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			log.Printf("upgrade from %s: %v", clientIP, err)
			return
		}
		metrics.activeConnections.Add(1)
		metrics.totalConnections.Add(1)
		defer metrics.activeConnections.Add(-1)

		p := newPeer(id, conn, scope)
		h.put(p)
		defer func() {
			pendingHostAuthRequests.removeSource(p)
			h.remove(id, p)
			_ = conn.Close()
		}()

		conn.SetReadLimit(maxMessageBytes)
		_ = conn.SetReadDeadline(time.Now().Add(pongWait))
		conn.SetPongHandler(func(string) error {
			return conn.SetReadDeadline(time.Now().Add(pongWait))
		})

		done := make(chan struct{})
		defer close(done)
		go func() {
			ticker := time.NewTicker(pingPeriod)
			defer ticker.Stop()
			for {
				select {
				case <-ticker.C:
					recheckID := id
					if p.controller {
						recheckID = p.allowedTarget
					}
					revoked, err := deviceRevoked(recheckID)
					if err != nil {
						log.Printf("device revocation check failed for %s: %v", recheckID, err)
						p.closeWithReason("device authorization unavailable")
						return
					}
					if revoked {
						log.Printf("disconnecting peer %s because target/device %s is revoked", id, recheckID)
						p.closeWithReason("device revoked")
						return
					}
					if err := p.ping(); err != nil {
						_ = conn.Close()
						return
					}
				case <-done:
					return
				}
			}
		}()

		_ = p.write(map[string]any{"type": "registered", "deviceId": id})
		flushPendingHostAuthRequests(h, p, &metrics)

		for {
			var msg envelope
			if err := conn.ReadJSON(&msg); err != nil {
				return
			}
			_ = conn.SetReadDeadline(time.Now().Add(pongWait))

			if !p.allowSignal() {
				log.Printf("signal rate limit exceeded by %s", id)
				return
			}

			if msg.Type == "ping" {
				_ = p.write(map[string]any{"type": "pong", "ts": time.Now().UnixMilli()})
				continue
			}
			if !allowedSignalType(msg.Type) {
				_ = p.write(map[string]any{"type": "error", "message": "unsupported signal type"})
				continue
			}
			if !validSessionID(msg.Session) {
				_ = p.write(map[string]any{"type": "error", "message": "valid session is required"})
				continue
			}
			if msg.Target == "" || !validDeviceID(msg.Target) {
				_ = p.write(map[string]any{"type": "error", "message": "valid target is required"})
				continue
			}
			if !p.canSignalTarget(msg.Target) {
				metrics.authFailures.Add(1)
				_ = p.write(map[string]any{"type": "error", "message": "target is outside controller authorization scope"})
				continue
			}

			targetRevoked, targetRevocationErr := deviceRevoked(msg.Target)
			if targetRevocationErr != nil {
				_ = p.write(map[string]any{"type": "error", "message": "device authorization temporarily unavailable"})
				continue
			}
			if targetRevoked {
				_ = p.write(map[string]any{"type": "peer-offline", "target": msg.Target})
				continue
			}

			target := h.get(msg.Target)
			if target == nil {
				if shouldQueuePendingHostAuth(p, msg.Type) {
					if pendingHostAuthRequests.enqueue(
						msg.Target,
						p,
						msg.Session,
						msg.Payload,
						time.Now(),
					) {
						metrics.pendingHostAuthQueued.Add(1)
					} else {
						metrics.pendingHostAuthDropped.Add(1)
					}
				}
				_ = p.write(map[string]any{"type": "peer-offline", "target": msg.Target})
				continue
			}

			claimedHostAuth := false
			if msg.Type == "auth-request" {
				if !hostAuthDispatches.claim(msg.Target, p, msg.Session, time.Now()) {
					metrics.hostAuthDuplicatesSuppressed.Add(1)
					continue
				}
				claimedHostAuth = true
			}

			forward := map[string]any{
				"type":    msg.Type,
				"from":    id,
				"session": msg.Session,
				"payload": msg.Payload,
			}
			if err := target.write(forward); err != nil {
				if claimedHostAuth {
					hostAuthDispatches.release(msg.Target, p, msg.Session)
					if shouldQueuePendingHostAuth(p, msg.Type) {
						if pendingHostAuthRequests.enqueue(
							msg.Target,
							p,
							msg.Session,
							msg.Payload,
							time.Now(),
						) {
							metrics.pendingHostAuthQueued.Add(1)
						} else {
							metrics.pendingHostAuthDropped.Add(1)
						}
					}
				}
				h.remove(msg.Target, target)
				_ = target.conn.Close()
				_ = p.write(map[string]any{"type": "peer-offline", "target": msg.Target})
				log.Printf("forward %s -> %s: %v", id, msg.Target, err)
			} else {
				metrics.messagesForwarded.Add(1)
			}
		}
	})

	addr := os.Getenv("DESKLINK_SIGNAL_ADDR")
	if addr == "" {
		addr = ":8080"
	}
	server := &http.Server{
		Addr:              addr,
		ReadHeaderTimeout: 5 * time.Second,
		ReadTimeout:       10 * time.Second,
		WriteTimeout:      10 * time.Second,
		IdleTimeout:       90 * time.Second,
		MaxHeaderBytes:    16 << 10,
	}

	shutdownSignal, stopSignals := osSignal.NotifyContext(
		context.Background(),
		os.Interrupt,
		syscall.SIGTERM,
	)
	defer stopSignals()

	serveErrors := make(chan error, 1)
	go func() {
		log.Printf("DeskLink signaling listening on %s", addr)
		serveErrors <- server.ListenAndServe()
	}()

	select {
	case err := <-serveErrors:
		if err != nil && err != http.ErrServerClosed {
			log.Printf("signaling server stopped unexpectedly: %v", err)
		}
		return
	case <-shutdownSignal.Done():
		log.Printf("DeskLink signaling shutting down")
	}

	shutdownContext, cancelShutdown := context.WithTimeout(context.Background(), 5*time.Second)
	defer cancelShutdown()
	if err := server.Shutdown(shutdownContext); err != nil {
		log.Printf("HTTP shutdown: %v", err)
	}

	// net/http does not own hijacked WebSocket connections after Upgrade, so
	// explicitly close them after the listener stops accepting new clients.
	h.closeAll("signaling service restarting")
}
