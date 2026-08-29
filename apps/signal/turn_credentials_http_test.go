package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

func TestTurnCredentialHandlerDisabledWithoutSecrets(t *testing.T) {
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "")
	t.Setenv("DESKLINK_TURN_AUTH_SECRET", "")

	req := httptest.NewRequest(http.MethodGet, "/api/v1/turn-credentials?deviceId=win-test", nil)
	res := httptest.NewRecorder()
	turnCredentialHandler().ServeHTTP(res, req)

	if res.Code != http.StatusNotFound {
		t.Fatalf("expected 404 when credential service is disabled, got %d", res.Code)
	}
}

func TestTurnCredentialHandlerRejectsInvalidSignalToken(t *testing.T) {
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "signal-secret")
	t.Setenv("DESKLINK_TURN_AUTH_SECRET", "turn-secret")

	req := httptest.NewRequest(http.MethodGet, "/api/v1/turn-credentials?deviceId=win-test", nil)
	req.Header.Set("Authorization", "Bearer invalid")
	res := httptest.NewRecorder()
	turnCredentialHandler().ServeHTTP(res, req)

	if res.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 for invalid token, got %d", res.Code)
	}
}

func TestTurnCredentialHandlerIssuesTemporaryCredentials(t *testing.T) {
	const signalSecret = "signal-secret"
	const turnSecret = "turn-secret"
	const deviceID = "win-test"
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", signalSecret)
	t.Setenv("DESKLINK_TURN_AUTH_SECRET", turnSecret)
	t.Setenv("DESKLINK_TURN_CREDENTIAL_TTL_SECONDS", "3600")

	token := mintSignalAuthToken(signalSecret, deviceID, time.Now().Add(time.Hour))
	req := httptest.NewRequest(http.MethodGet, "/api/v1/turn-credentials?deviceId="+deviceID, nil)
	req.Header.Set("Authorization", "Bearer "+token)
	res := httptest.NewRecorder()
	turnCredentialHandler().ServeHTTP(res, req)

	if res.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", res.Code, res.Body.String())
	}
	if cacheControl := res.Header().Get("Cache-Control"); cacheControl != "no-store" {
		t.Fatalf("expected no-store response, got %q", cacheControl)
	}

	var credentials turnCredentials
	if err := json.NewDecoder(res.Body).Decode(&credentials); err != nil {
		t.Fatalf("decode credentials: %v", err)
	}
	if !strings.HasSuffix(credentials.Username, ":"+deviceID) {
		t.Fatalf("TURN username is not bound to device: %q", credentials.Username)
	}
	if credentials.Password == "" {
		t.Fatal("TURN password is empty")
	}
	if credentials.ExpiresAt <= time.Now().Unix() {
		t.Fatalf("TURN credential already expired: %d", credentials.ExpiresAt)
	}
}

func TestTurnCredentialHandlerCORSUsesAllowedOrigin(t *testing.T) {
	const signalSecret = "signal-secret"
	const deviceID = "web-test"
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", signalSecret)
	t.Setenv("DESKLINK_TURN_AUTH_SECRET", "turn-secret")
	t.Setenv("DESKLINK_ALLOWED_ORIGINS", "https://controller.example.com")
	t.Setenv("DESKLINK_ALLOW_ANY_ORIGIN", "0")

	token := mintSignalAuthToken(signalSecret, deviceID, time.Now().Add(time.Hour))
	req := httptest.NewRequest(http.MethodGet, "/api/v1/turn-credentials?deviceId="+deviceID, nil)
	req.Header.Set("Authorization", "Bearer "+token)
	req.Header.Set("Origin", "https://controller.example.com")
	res := httptest.NewRecorder()
	turnCredentialHandler().ServeHTTP(res, req)

	if res.Code != http.StatusOK {
		t.Fatalf("expected 200 for allowed origin, got %d", res.Code)
	}
	if got := res.Header().Get("Access-Control-Allow-Origin"); got != "https://controller.example.com" {
		t.Fatalf("unexpected Access-Control-Allow-Origin: %q", got)
	}
}
