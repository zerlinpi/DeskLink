package main

import (
	"bytes"
	"errors"
	"net/http"
	"os"
	"os/exec"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

func startSignalRevocationE2EServer(t *testing.T, revocationFile string) signalE2EServer {
	t.Helper()
	address := reserveSignalE2EAddress(t)
	secret := "desklink-revocation-e2e-secret"

	cmd := exec.Command(os.Args[0], "-test.run=^TestSignalServerHelperProcess$")
	cmd.Env = append(os.Environ(),
		signalE2EHelperEnv+"=1",
		"DESKLINK_SIGNAL_ADDR="+address,
		"DESKLINK_SIGNAL_AUTH_SECRET="+secret,
		"DESKLINK_ALLOW_ANY_ORIGIN=1",
		"DESKLINK_ALLOWED_ORIGINS=",
		"DESKLINK_TRUST_PROXY_HEADERS=0",
		"DESKLINK_CONTROLLER_CREDENTIALS_FILE=",
		"DESKLINK_DEVICE_CREDENTIALS_FILE=",
		"DESKLINK_DEVICE_AUTH_SECRET=",
		"DESKLINK_REVOKED_DEVICE_IDS=",
		"DESKLINK_REVOKED_DEVICE_IDS_FILE="+revocationFile,
		"DESKLINK_METRICS_TOKEN=",
	)
	var logs bytes.Buffer
	cmd.Stdout = &logs
	cmd.Stderr = &logs
	if err := cmd.Start(); err != nil {
		t.Fatalf("start revocation signal test server: %v", err)
	}

	var stopOnce sync.Once
	stop := func() {
		stopOnce.Do(func() {
			if cmd.Process != nil {
				_ = cmd.Process.Kill()
				_, _ = cmd.Process.Wait()
			}
		})
	}
	t.Cleanup(stop)

	httpEndpoint := "http://" + address
	client := &http.Client{Timeout: 250 * time.Millisecond}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		response, err := client.Get(httpEndpoint + "/healthz")
		if err == nil {
			_ = response.Body.Close()
			if response.StatusCode == http.StatusOK {
				return signalE2EServer{
					wsEndpoint:   "ws://" + address + "/ws",
					httpEndpoint: httpEndpoint,
					signalSecret: secret,
				}
			}
		}
		time.Sleep(25 * time.Millisecond)
	}

	stop()
	t.Fatalf("revocation signal test server did not become healthy:\n%s", logs.String())
	return signalE2EServer{}
}

func TestSignalLiveDeviceRevocationEndToEnd(t *testing.T) {
	revocationFile := t.TempDir() + "/revoked-devices.txt"
	if err := os.WriteFile(revocationFile, nil, 0o600); err != nil {
		t.Fatalf("create revocation file: %v", err)
	}
	server := startSignalRevocationE2EServer(t, revocationFile)

	controllerID := "controller-e2e-revocation"
	hostID := "host-e2e-revocation"
	controllerToken := mintControllerSignalToken(
		server.signalSecret,
		controllerID,
		hostID,
		time.Now().Add(5*time.Minute),
	)
	controller := dialSignalE2EController(t, server.wsEndpoint, controllerID, controllerToken)

	// device-revoked is server-owned. A client must not be able to synthesize a
	// forced disconnect event for another authenticated peer.
	if err := controller.WriteJSON(envelope{
		Type:    "device-revoked",
		Target:  hostID,
		Session: "session-e2e-revocation-forge",
	}); err != nil {
		t.Fatalf("write forged device-revoked: %v", err)
	}
	forgedResponse := readSignalE2EMessage(t, controller, 2*time.Second)
	if forgedResponse.Type != "error" || !strings.Contains(forgedResponse.Message, "unsupported signal type") {
		t.Fatalf("client-forged device-revoked was not rejected: %+v", forgedResponse)
	}

	if err := os.WriteFile(revocationFile, []byte(hostID+"\n"), 0o600); err != nil {
		t.Fatalf("revoke target device: %v", err)
	}
	writeSignalE2EAuthRequest(t, controller, hostID, "session-e2e-revocation-live")

	revoked := readSignalE2EMessage(t, controller, 2*time.Second)
	if revoked.Type != "device-revoked" || revoked.Target != hostID {
		t.Fatalf("expected server device-revoked event, got %+v", revoked)
	}

	if err := controller.SetReadDeadline(time.Now().Add(2 * time.Second)); err != nil {
		t.Fatalf("set close deadline: %v", err)
	}
	var unexpected signalE2EMessage
	err := controller.ReadJSON(&unexpected)
	if err == nil {
		t.Fatalf("expected controller socket close after revocation, got %+v", unexpected)
	}
	var closeError *websocket.CloseError
	if !errors.As(err, &closeError) {
		t.Fatalf("expected websocket close after revocation, got %v", err)
	}
	if closeError.Text != "device revoked" {
		t.Fatalf("unexpected revocation close reason %q", closeError.Text)
	}
}
