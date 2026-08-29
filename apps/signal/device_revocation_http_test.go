package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestRevokedDeviceCannotExchangeSignalToken(t *testing.T) {
	const deviceID = "revoked-pc"
	const deviceSecret = "device-secret"
	t.Setenv("DESKLINK_DEVICE_AUTH_SECRET", deviceSecret)
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "signal-secret")
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", deviceID)
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS_FILE", "")

	req := httptest.NewRequest(http.MethodGet, "/api/v1/signal-token?deviceId="+deviceID, nil)
	req.Header.Set("Authorization", "Bearer "+deriveDeviceCredential(deviceSecret, deviceID))
	response := httptest.NewRecorder()
	signalTokenHandler().ServeHTTP(response, req)

	if response.Code != http.StatusForbidden {
		t.Fatalf("expected 403 for revoked device, got %d", response.Code)
	}
}

func TestRevokedDeviceCannotExchangeTurnCredentials(t *testing.T) {
	const deviceID = "revoked-pc"
	const signalSecret = "signal-secret"
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", signalSecret)
	t.Setenv("DESKLINK_TURN_AUTH_SECRET", "turn-secret")
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", deviceID)
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS_FILE", "")

	token := mintSignalAuthToken(signalSecret, deviceID, time.Now().Add(10*time.Minute))
	req := httptest.NewRequest(http.MethodGet, "/api/v1/turn-credentials?deviceId="+deviceID, nil)
	req.Header.Set("Authorization", "Bearer "+token)
	response := httptest.NewRecorder()
	turnCredentialHandler().ServeHTTP(response, req)

	if response.Code != http.StatusForbidden {
		t.Fatalf("expected 403 for revoked device TURN credentials, got %d", response.Code)
	}
}
