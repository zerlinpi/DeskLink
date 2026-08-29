package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestDeviceCredentialIsBoundToDeviceID(t *testing.T) {
	credential := deriveDeviceCredential("device-secret", "host-a")
	if credential == "" {
		t.Fatal("expected credential")
	}
	if !validateDeviceCredential("device-secret", "host-a", credential) {
		t.Fatal("expected credential to validate for host-a")
	}
	if validateDeviceCredential("device-secret", "host-b", credential) {
		t.Fatal("credential must not validate for a different device")
	}
	if validateDeviceCredential("different-secret", "host-a", credential) {
		t.Fatal("credential must not validate under another server secret")
	}
}

func TestSignalTokenHandlerDisabledWithoutSecrets(t *testing.T) {
	t.Setenv("DESKLINK_DEVICE_AUTH_SECRET", "")
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "")

	req := httptest.NewRequest(http.MethodGet, "/api/v1/signal-token?deviceId=host-a", nil)
	rec := httptest.NewRecorder()
	signalTokenHandler()(rec, req)
	if rec.Code != http.StatusNotFound {
		t.Fatalf("expected 404, got %d", rec.Code)
	}
}

func TestSignalTokenHandlerRejectsWrongCredential(t *testing.T) {
	t.Setenv("DESKLINK_DEVICE_AUTH_SECRET", "device-secret")
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "signal-secret")

	req := httptest.NewRequest(http.MethodGet, "/api/v1/signal-token?deviceId=host-a", nil)
	req.Header.Set("Authorization", "Bearer wrong")
	rec := httptest.NewRecorder()
	signalTokenHandler()(rec, req)
	if rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401, got %d", rec.Code)
	}
}

func TestSignalTokenHandlerIssuesShortLivedRegistrationToken(t *testing.T) {
	const deviceID = "host-a"
	const deviceSecret = "device-secret"
	const signalSecret = "signal-secret"
	t.Setenv("DESKLINK_DEVICE_AUTH_SECRET", deviceSecret)
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", signalSecret)
	t.Setenv("DESKLINK_SIGNAL_TOKEN_TTL", "10m")

	credential := deriveDeviceCredential(deviceSecret, deviceID)
	req := httptest.NewRequest(http.MethodGet, "/api/v1/signal-token?deviceId="+deviceID, nil)
	req.Header.Set("Authorization", "Bearer "+credential)
	rec := httptest.NewRecorder()
	before := time.Now()
	signalTokenHandler()(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}

	var response issuedSignalToken
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if response.Token == "" || response.ExpiresAt == 0 {
		t.Fatalf("incomplete token response: %+v", response)
	}
	if !validateSignalAuthToken(signalSecret, deviceID, response.Token, before.Add(time.Second)) {
		t.Fatal("issued token did not validate")
	}

	ttl := time.Unix(response.ExpiresAt, 0).Sub(before)
	if ttl < 9*time.Minute || ttl > 11*time.Minute {
		t.Fatalf("unexpected TTL: %v", ttl)
	}
	if rec.Header().Get("Cache-Control") != "no-store" {
		t.Fatalf("expected no-store cache control")
	}
}

func TestIssuedSignalTokenTTLRejectsUnsafeValue(t *testing.T) {
	t.Setenv("DESKLINK_SIGNAL_TOKEN_TTL", "24h")
	if got := issuedSignalTokenTTL(); got != defaultIssuedSignalTokenTTL {
		t.Fatalf("expected fallback TTL, got %v", got)
	}
}
