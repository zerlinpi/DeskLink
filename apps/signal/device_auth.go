package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"net/http"
	"os"
	"strings"
	"time"
)

const (
	defaultIssuedSignalTokenTTL = 15 * time.Minute
	maxIssuedSignalTokenTTL     = time.Hour
)

type issuedSignalToken struct {
	Token     string `json:"token"`
	ExpiresAt int64  `json:"expiresAt"`
}

// deriveDeviceCredential creates the long-lived bootstrap credential for one
// exact device ID. It is intentionally distinct from the short-lived signaling
// registration token and must never be sent in WebSocket URLs or ICE signaling.
func deriveDeviceCredential(secret, deviceID string) string {
	if secret == "" || !validDeviceID(deviceID) {
		return ""
	}
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte("DeskLink device credential\n" + deviceID))
	return "dc1." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
}

func validateDeviceCredential(secret, deviceID, credential string) bool {
	expected := deriveDeviceCredential(secret, deviceID)
	if expected == "" || credential == "" {
		return false
	}
	return hmac.Equal([]byte(expected), []byte(credential))
}

func issuedSignalTokenTTL() time.Duration {
	value := strings.TrimSpace(os.Getenv("DESKLINK_SIGNAL_TOKEN_TTL"))
	if value == "" {
		return defaultIssuedSignalTokenTTL
	}
	parsed, err := time.ParseDuration(value)
	if err != nil || parsed <= 0 || parsed > maxIssuedSignalTokenTTL {
		return defaultIssuedSignalTokenTTL
	}
	return parsed
}

func signalTokenHandler() http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			w.Header().Set("Allow", http.MethodGet)
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		deviceSecret := os.Getenv("DESKLINK_DEVICE_AUTH_SECRET")
		signalSecret := os.Getenv("DESKLINK_SIGNAL_AUTH_SECRET")
		if deviceSecret == "" || signalSecret == "" {
			http.NotFound(w, r)
			return
		}

		deviceID := r.URL.Query().Get("deviceId")
		if !validDeviceID(deviceID) {
			http.Error(w, "valid deviceId is required", http.StatusBadRequest)
			return
		}
		if !validateDeviceCredential(deviceSecret, deviceID, bearerToken(r)) {
			http.Error(w, "unauthorized device credential", http.StatusUnauthorized)
			return
		}

		ttl := issuedSignalTokenTTL()
		expiresAt := time.Now().Add(ttl)
		token := mintSignalAuthToken(signalSecret, deviceID, expiresAt)
		if token == "" {
			http.Error(w, "unable to issue signal token", http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Pragma", "no-cache")
		_ = json.NewEncoder(w).Encode(issuedSignalToken{
			Token:     token,
			ExpiresAt: expiresAt.Unix(),
		})
	}
}
