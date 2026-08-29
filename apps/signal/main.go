package main

import (
	"encoding/json"
	"log"
	"math"
	"net/http"
	"os"
	"strings"
	"sync"
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
	id   string
	conn *websocket.Conn
	mu   sync.Mutex

	rateMu     sync.Mutex
	tokens     float64
	lastRefill time.Time
}

type hub struct {
	mu    sync.RWMutex
	peers map[string]*peer
}

func newHub() *hub { return &hub{peers: make(map[string]*peer)} }

func newPeer(id string, conn *websocket.Conn) *peer {
	return &peer{
		id:         id,
		conn:       conn,
		tokens:     signalBurst,
		lastRefill: time.Now(),
	}
}

func (h *hub) put(p *peer) {
	h.mu.Lock()
	defer h.mu.Unlock()
	if old := h.peers[p.id]; old != nil {
		_ = old.conn.Close()
	}
	h.peers[p.id] = p
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
	case "offer", "answer", "ice", "session-request", "session-accept":
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
	CheckOrigin: func(r *http.Request) bool {
		return originAllowed(r.Header.Get("Origin"))
	},
}

func main() {
	h := newHub()
	guard := newIPGuard()
	var metrics signalMetrics
	registerMetricsHandler(&metrics)

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
		if !signalRegistrationAuthorized(r, id) {
			guard.authFailed(clientIP, time.Now())
			metrics.authFailures.Add(1)
			http.Error(w, "unauthorized device registration", http.StatusUnauthorized)
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

		p := newPeer(id, conn)
		h.put(p)
		defer func() {
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

			target := h.get(msg.Target)
			if target == nil {
				_ = p.write(map[string]any{"type": "peer-offline", "target": msg.Target})
				continue
			}

			forward := map[string]any{
				"type":    msg.Type,
				"from":    id,
				"session": msg.Session,
				"payload": msg.Payload,
			}
			if err := target.write(forward); err != nil {
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
		IdleTimeout:       90 * time.Second,
	}
	log.Printf("DeskLink signaling listening on %s", addr)
	log.Fatal(server.ListenAndServe())
}
