package main

import (
	"strings"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func TestRegistrationScopeRetainsValidatedTokenExpiry(t *testing.T) {
	const secret = "registration-expiry-secret"
	now := time.Unix(1_800_000_000, 0)
	expiresAt := now.Add(15 * time.Minute)

	hostToken := mintSignalAuthToken(secret, "office-pc", expiresAt)
	hostScope, ok := registrationScopeForToken(secret, "office-pc", hostToken, now)
	if !ok || hostScope.Controller || !hostScope.ExpiresAt.Equal(expiresAt) {
		t.Fatalf("unexpected host registration scope: %+v ok=%v", hostScope, ok)
	}

	controllerToken := mintControllerSignalToken(
		secret,
		"web-expiry-test",
		"office-pc",
		expiresAt,
	)
	controllerScope, ok := registrationScopeForToken(
		secret,
		"web-expiry-test",
		controllerToken,
		now,
	)
	if !ok || !controllerScope.Controller || controllerScope.AllowedTarget != "office-pc" ||
		!controllerScope.ExpiresAt.Equal(expiresAt) {
		t.Fatalf("unexpected controller registration scope: %+v ok=%v", controllerScope, ok)
	}
}

func expectSignalE2ERegistrationExpiry(t *testing.T, conn *websocket.Conn) {
	t.Helper()
	if err := conn.SetReadDeadline(time.Now().Add(7 * time.Second)); err != nil {
		t.Fatalf("set expiry read deadline: %v", err)
	}
	_, _, err := conn.ReadMessage()
	if err == nil {
		t.Fatal("registration socket stayed open after token expiry")
	}
	closeError, ok := err.(*websocket.CloseError)
	if !ok {
		t.Fatalf("expected WebSocket close error after token expiry, got %T: %v", err, err)
	}
	if !strings.Contains(closeError.Text, "registration token expired") {
		t.Fatalf("unexpected token-expiry close reason: %q", closeError.Text)
	}
}

func TestSignalWebSocketRegistrationExpiryEndToEnd(t *testing.T) {
	server := startSignalE2EServer(t)

	t.Run("controller socket expires with ct1", func(t *testing.T) {
		controllerID := "controller-e2e-expiry"
		targetID := "host-e2e-expiry-target"
		token := mintControllerSignalToken(
			server.signalSecret,
			controllerID,
			targetID,
			time.Now().Add(4*time.Second),
		)
		controller := dialSignalE2EController(t, server.wsEndpoint, controllerID, token)
		expectSignalE2ERegistrationExpiry(t, controller)
	})

	t.Run("host socket expires with signal token", func(t *testing.T) {
		hostID := "host-e2e-expiry"
		token := mintSignalAuthToken(
			server.signalSecret,
			hostID,
			time.Now().Add(4*time.Second),
		)
		host := dialSignalE2EHost(t, server.wsEndpoint, hostID, token)
		expectSignalE2ERegistrationExpiry(t, host)
	})
}
