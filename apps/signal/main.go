package main

import (
	"encoding/json"
	"log"
	"net/http"
	"os"
	"sync"
	"time"

	"github.com/gorilla/websocket"
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
}

type hub struct {
	mu    sync.RWMutex
	peers map[string]*peer
}

func newHub() *hub { return &hub{peers: make(map[string]*peer)} }

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

func (p *peer) write(v any) error {
	p.mu.Lock()
	defer p.mu.Unlock()
	_ = p.conn.SetWriteDeadline(time.Now().Add(8 * time.Second))
	return p.conn.WriteJSON(v)
}

var upgrader = websocket.Upgrader{
	ReadBufferSize:  4096,
	WriteBufferSize: 4096,
	CheckOrigin: func(r *http.Request) bool {
		// Development default. In production, restrict this to the controller origins.
		return os.Getenv("DESKLINK_ALLOW_ANY_ORIGIN") == "1" || r.Header.Get("Origin") == ""
	},
}

func main() {
	h := newHub()

	http.HandleFunc("/healthz", func(w http.ResponseWriter, _ *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		_, _ = w.Write([]byte(`{"ok":true}`))
	})

	http.HandleFunc("/ws", func(w http.ResponseWriter, r *http.Request) {
		id := r.URL.Query().Get("deviceId")
		if id == "" {
			http.Error(w, "deviceId is required", http.StatusBadRequest)
			return
		}

		conn, err := upgrader.Upgrade(w, r, nil)
		if err != nil {
			log.Printf("upgrade: %v", err)
			return
		}
		p := &peer{id: id, conn: conn}
		h.put(p)
		defer func() {
			h.remove(id, p)
			_ = conn.Close()
		}()

		conn.SetReadLimit(1 << 20)
		_ = p.write(map[string]any{"type": "registered", "deviceId": id})

		for {
			_ = conn.SetReadDeadline(time.Now().Add(70 * time.Second))
			var msg envelope
			if err := conn.ReadJSON(&msg); err != nil {
				return
			}

			if msg.Type == "ping" {
				_ = p.write(map[string]any{"type": "pong", "ts": time.Now().UnixMilli()})
				continue
			}
			if msg.Target == "" {
				_ = p.write(map[string]any{"type": "error", "message": "target is required"})
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
			}
		}
	})

	addr := os.Getenv("DESKLINK_SIGNAL_ADDR")
	if addr == "" {
		addr = ":8080"
	}
	log.Printf("DeskLink signaling listening on %s", addr)
	log.Fatal(http.ListenAndServe(addr, nil))
}
