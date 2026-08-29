package main

import (
	"net"
	"net/http"
	"os"
	"strings"
	"sync"
	"time"
)

const (
	defaultHandshakeRate      = 4.0
	defaultHandshakeBurst     = 16.0
	defaultMaxConnectionsPerIP = 12
	authFailureBlockThreshold = 6
	authFailureBlockDuration  = 45 * time.Second
	ipStateTTL                = 10 * time.Minute
)

type ipGuardState struct {
	tokens       float64
	lastRefill   time.Time
	active       int
	authFailures int
	blockedUntil time.Time
	lastSeen     time.Time
}

type ipGuard struct {
	mu        sync.Mutex
	states    map[string]*ipGuardState
	lastSweep time.Time
}

func newIPGuard() *ipGuard {
	return &ipGuard{
		states:    make(map[string]*ipGuardState),
		lastSweep: time.Now(),
	}
}

func (g *ipGuard) reserve(ip string, now time.Time) bool {
	g.mu.Lock()
	defer g.mu.Unlock()

	g.sweepLocked(now)
	state := g.stateLocked(ip, now)
	state.lastSeen = now

	if now.Before(state.blockedUntil) {
		return false
	}
	if state.active >= defaultMaxConnectionsPerIP {
		return false
	}

	elapsed := now.Sub(state.lastRefill).Seconds()
	if elapsed > 0 {
		state.tokens += elapsed * defaultHandshakeRate
		if state.tokens > defaultHandshakeBurst {
			state.tokens = defaultHandshakeBurst
		}
		state.lastRefill = now
	}
	if state.tokens < 1 {
		return false
	}

	state.tokens--
	state.active++
	return true
}

func (g *ipGuard) release(ip string, now time.Time) {
	g.mu.Lock()
	defer g.mu.Unlock()
	state := g.states[ip]
	if state == nil {
		return
	}
	if state.active > 0 {
		state.active--
	}
	state.lastSeen = now
}

func (g *ipGuard) authFailed(ip string, now time.Time) {
	g.mu.Lock()
	defer g.mu.Unlock()
	state := g.stateLocked(ip, now)
	state.lastSeen = now
	state.authFailures++
	if state.authFailures >= authFailureBlockThreshold {
		state.blockedUntil = now.Add(authFailureBlockDuration)
		state.authFailures = 0
	}
}

func (g *ipGuard) authSucceeded(ip string, now time.Time) {
	g.mu.Lock()
	defer g.mu.Unlock()
	state := g.stateLocked(ip, now)
	state.lastSeen = now
	state.authFailures = 0
	state.blockedUntil = time.Time{}
}

func (g *ipGuard) stateLocked(ip string, now time.Time) *ipGuardState {
	state := g.states[ip]
	if state != nil {
		return state
	}
	state = &ipGuardState{
		tokens:     defaultHandshakeBurst,
		lastRefill: now,
		lastSeen:   now,
	}
	g.states[ip] = state
	return state
}

func (g *ipGuard) sweepLocked(now time.Time) {
	if now.Sub(g.lastSweep) < time.Minute {
		return
	}
	g.lastSweep = now
	for ip, state := range g.states {
		if state.active == 0 && now.Sub(state.lastSeen) > ipStateTTL {
			delete(g.states, ip)
		}
	}
}

func requestClientIP(r *http.Request) string {
	if os.Getenv("DESKLINK_TRUST_PROXY_HEADERS") == "1" {
		for _, header := range []string{"CF-Connecting-IP", "X-Real-IP"} {
			if value := strings.TrimSpace(r.Header.Get(header)); value != "" {
				return value
			}
		}
		if forwarded := r.Header.Get("X-Forwarded-For"); forwarded != "" {
			if first := strings.TrimSpace(strings.Split(forwarded, ",")[0]); first != "" {
				return first
			}
		}
	}

	host, _, err := net.SplitHostPort(r.RemoteAddr)
	if err == nil && host != "" {
		return host
	}
	if r.RemoteAddr != "" {
		return r.RemoteAddr
	}
	return "unknown"
}
