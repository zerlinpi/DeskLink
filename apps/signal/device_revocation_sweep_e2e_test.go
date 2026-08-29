package main

import (
	"errors"
	"os"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func expectSignalE2ERevokedAndClosed(
	t *testing.T,
	conn *websocket.Conn,
	target string,
	timeout time.Duration,
) {
	t.Helper()
	message := readSignalE2EMessage(t, conn, timeout)
	if message.Type != "device-revoked" || message.Target != target {
		t.Fatalf("expected device-revoked for %s, got %+v", target, message)
	}

	if err := conn.SetReadDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("set revocation close deadline: %v", err)
	}
	var unexpected signalE2EMessage
	err := conn.ReadJSON(&unexpected)
	if err == nil {
		t.Fatalf("expected socket close after device-revoked, got %+v", unexpected)
	}
	var closeError *websocket.CloseError
	if !errors.As(err, &closeError) {
		t.Fatalf("expected websocket close after revocation, got %v", err)
	}
	if closeError.Text != "device revoked" {
		t.Fatalf("unexpected revocation close reason %q", closeError.Text)
	}
}

func TestSignalIdleControllerRevocationSweepEndToEnd(t *testing.T) {
	revocationFile := t.TempDir() + "/revoked-devices.txt"
	if err := os.WriteFile(revocationFile, nil, 0o600); err != nil {
		t.Fatalf("create revocation file: %v", err)
	}
	server := startSignalRevocationE2EServer(t, revocationFile)

	controllerID := "controller-e2e-idle-revocation"
	hostID := "host-e2e-idle-revocation"
	controllerToken := mintControllerSignalToken(
		server.signalSecret,
		controllerID,
		hostID,
		time.Now().Add(5*time.Minute),
	)
	controller := dialSignalE2EController(t, server.wsEndpoint, controllerID, controllerToken)

	// Do not send another signaling message after this write. The shared server
	// sweep must discover the revoked target and terminate the idle controller.
	if err := os.WriteFile(revocationFile, []byte(hostID+"\n"), 0o600); err != nil {
		t.Fatalf("revoke idle target device: %v", err)
	}
	expectSignalE2ERevokedAndClosed(
		t,
		controller,
		hostID,
		revocationSweepPeriod+3*time.Second,
	)
}

func TestSignalIdleHostRevocationSweepEndToEnd(t *testing.T) {
	revocationFile := t.TempDir() + "/revoked-devices.txt"
	if err := os.WriteFile(revocationFile, nil, 0o600); err != nil {
		t.Fatalf("create revocation file: %v", err)
	}
	server := startSignalRevocationE2EServer(t, revocationFile)

	hostID := "host-e2e-idle-self-revocation"
	hostToken := mintSignalAuthToken(server.signalSecret, hostID, time.Now().Add(5*time.Minute))
	host := dialSignalE2EHost(t, server.wsEndpoint, hostID, hostToken)

	if err := os.WriteFile(revocationFile, []byte(hostID+"\n"), 0o600); err != nil {
		t.Fatalf("revoke idle host: %v", err)
	}
	expectSignalE2ERevokedAndClosed(
		t,
		host,
		hostID,
		revocationSweepPeriod+3*time.Second,
	)
}
