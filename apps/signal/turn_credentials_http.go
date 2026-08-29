package main

import (
	"encoding/json"
	"net/http"
	"os"
	"strconv"
	"strings"
	"time"
)

const defaultTurnCredentialTTL = 12 * time.Hour

func turnCredentialTTL() time.Duration {
	value := strings.TrimSpace(os.Getenv("DESKLINK_TURN_CREDENTIAL_TTL_SECONDS"))
	if value == "" {
		return defaultTurnCredentialTTL
	}
	seconds, err := strconv.ParseInt(value, 10, 64)
	if err != nil {
		return defaultTurnCredentialTTL
	}
	seconds = max(60, min(seconds, int64(maxTurnCredentialTTL/time.Second)))
	return time.Duration(seconds) * time.Second
}

func bearerToken(r *http.Request) string {
	const prefix = "Bearer "
	header := strings.TrimSpace(r.Header.Get("Authorization"))
	if len(header) <= len(prefix) || !strings.EqualFold(header[:len(prefix)], prefix) {
		return ""
	}
	return strings.TrimSpace(header[len(prefix):])
}

func setCredentialCORS(w http.ResponseWriter, r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return true
	}
	if !originAllowed(origin) {
		return false
	}
	w.Header().Set("Access-Control-Allow-Origin", origin)
	w.Header().Set("Vary", "Origin")
	w.Header().Set("Access-Control-Allow-Headers", "Authorization, Content-Type")
	w.Header().Set("Access-Control-Allow-Methods", "GET, OPTIONS")
	return true
}

func turnCredentialHandler() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		signalSecret := os.Getenv("DESKLINK_SIGNAL_AUTH_SECRET")
		if signalSecret == "" || os.Getenv("DESKLINK_TURN_AUTH_SECRET") == "" {
			http.NotFound(w, r)
			return
		}

		if !setCredentialCORS(w, r) {
			http.Error(w, "origin not allowed", http.StatusForbidden)
			return
		}
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", "GET, OPTIONS")
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		peerID := r.URL.Query().Get("deviceId")
		if !validDeviceID(peerID) {
			http.Error(w, "valid deviceId is required", http.StatusBadRequest)
			return
		}

		now := time.Now()
		scope, authorized := registrationScopeForToken(
			signalSecret,
			peerID,
			bearerToken(r),
			now,
		)
		if !authorized {
			http.Error(w, "unauthorized", http.StatusUnauthorized)
			return
		}

		// Host tokens are bound directly to the host peer ID. Controller tokens
		// are bound to an ephemeral browser peer and a separate target device; in
		// that case revocation must follow the authorized target, not the web ID.
		revocationID := peerID
		if scope.Controller {
			revocationID = scope.AllowedTarget
		}
		revoked, err := deviceRevoked(revocationID)
		if err != nil {
			http.Error(w, "device revocation state unavailable", http.StatusServiceUnavailable)
			return
		}
		if revoked {
			http.Error(w, "device revoked", http.StatusForbidden)
			return
		}

		credentials, err := mintTurnCredentials(
			os.Getenv("DESKLINK_TURN_AUTH_SECRET"),
			peerID,
			turnCredentialTTL(),
			now,
		)
		if err != nil {
			http.Error(w, "unable to issue TURN credentials", http.StatusServiceUnavailable)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Pragma", "no-cache")
		_ = json.NewEncoder(w).Encode(credentials)
	}
}
