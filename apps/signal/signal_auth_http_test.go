package main

import (
	"net/http/httptest"
	"testing"
	"time"
)

func TestSignalRegistrationAuthorizationIsBackwardCompatibleWithoutSecret(t *testing.T) {
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "")
	req := httptest.NewRequest("GET", "http://signal.local/ws?deviceId=office-pc", nil)
	if !signalRegistrationAuthorized(req, "office-pc") {
		t.Fatal("development registration should remain allowed when auth secret is unset")
	}
}

func TestSignalRegistrationAuthorizationRequiresMatchingTokenWhenEnabled(t *testing.T) {
	const secret = "signal-registration-secret"
	const deviceID = "office-pc"
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", secret)

	expiresAt := time.Now().Add(10 * time.Minute).Truncate(time.Second)
	token := mintSignalAuthToken(secret, deviceID, expiresAt)

	good := httptest.NewRequest(
		"GET",
		"http://signal.local/ws?deviceId="+deviceID+"&auth="+token,
		nil,
	)
	if !signalRegistrationAuthorized(good, deviceID) {
		t.Fatal("valid registration token was rejected")
	}

	missing := httptest.NewRequest("GET", "http://signal.local/ws?deviceId="+deviceID, nil)
	if signalRegistrationAuthorized(missing, deviceID) {
		t.Fatal("missing registration token was accepted")
	}

	wrongDevice := httptest.NewRequest(
		"GET",
		"http://signal.local/ws?deviceId=other-pc&auth="+token,
		nil,
	)
	if signalRegistrationAuthorized(wrongDevice, "other-pc") {
		t.Fatal("token was accepted for a different device ID")
	}
}
