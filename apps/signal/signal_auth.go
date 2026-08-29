package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"net/http"
	"strconv"
	"strings"
	"time"
)

const maxSignalAuthLifetime = 24 * time.Hour

// Signal registration tokens are deliberately URL-safe because browser
// WebSocket APIs cannot attach arbitrary Authorization headers. Format:
//
//   <unix-expiry>.<base64url(HMAC-SHA256(secret, deviceID + "\n" + expiry))>
//
// These tokens are used by hosts and remain unrestricted at the signaling layer.
// Browser controllers use the separate ct1 target-scoped token format.
func mintSignalAuthToken(secret, deviceID string, expiresAt time.Time) string {
	if secret == "" || !validDeviceID(deviceID) {
		return ""
	}
	expiry := strconv.FormatInt(expiresAt.Unix(), 10)
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte(deviceID + "\n" + expiry))
	signature := base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	return expiry + "." + signature
}

func validateSignalAuthToken(secret, deviceID, token string, now time.Time) bool {
	if secret == "" || !validDeviceID(deviceID) || token == "" {
		return false
	}

	parts := strings.SplitN(token, ".", 2)
	if len(parts) != 2 || parts[0] == "" || parts[1] == "" {
		return false
	}

	expiresUnix, err := strconv.ParseInt(parts[0], 10, 64)
	if err != nil {
		return false
	}
	expiresAt := time.Unix(expiresUnix, 0)
	if !expiresAt.After(now) || expiresAt.Sub(now) > maxSignalAuthLifetime {
		return false
	}

	expected := mintSignalAuthToken(secret, deviceID, expiresAt)
	if expected == "" {
		return false
	}
	expectedParts := strings.SplitN(expected, ".", 2)
	if len(expectedParts) != 2 {
		return false
	}

	providedSig, err := base64.RawURLEncoding.DecodeString(parts[1])
	if err != nil {
		return false
	}
	expectedSig, err := base64.RawURLEncoding.DecodeString(expectedParts[1])
	if err != nil {
		return false
	}
	return hmac.Equal(providedSig, expectedSig)
}

// Retained for existing tests/callers. The richer scope is obtained through
// signalRegistrationScopeForRequest and is enforced by the WebSocket handler.
func signalRegistrationAuthorized(r *http.Request, deviceID string) bool {
	_, ok := signalRegistrationScopeForRequest(r, deviceID)
	return ok
}
