package main

import (
	"bytes"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"os"
	"os/exec"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

const signalE2EHelperEnv = "DESKLINK_SIGNAL_E2E_HELPER"

type signalE2EMessage struct {
	Type     string          `json:"type"`
	DeviceID string          `json:"deviceId,omitempty"`
	From     string          `json:"from,omitempty"`
	Target   string          `json:"target,omitempty"`
	Session  string          `json:"session,omitempty"`
	Payload  json.RawMessage `json:"payload,omitempty"`
	Message  string          `json:"message,omitempty"`
}

// TestSignalServerHelperProcess is launched in a child test process so the E2E
// test exercises the real main() HTTP/WebSocket server without restructuring the
// production listener around test-only hooks.
func TestSignalServerHelperProcess(t *testing.T) {
	if os.Getenv(signalE2EHelperEnv) != "1" {
		return
	}
	main()
}

func reserveSignalE2EAddress(t *testing.T) string {
	t.Helper()
	listener, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatalf("reserve signal test port: %v", err)
	}
	address := listener.Addr().String()
	if err := listener.Close(); err != nil {
		t.Fatalf("release signal test port: %v", err)
	}
	return address
}

func startSignalE2EServer(t *testing.T) (string, string) {
	t.Helper()
	address := reserveSignalE2EAddress(t)
	secret := "desklink-signal-e2e-secret"

	cmd := exec.Command(os.Args[0], "-test.run=^TestSignalServerHelperProcess$")
	cmd.Env = append(os.Environ(),
		signalE2EHelperEnv+"=1",
		"DESKLINK_SIGNAL_ADDR="+address,
		"DESKLINK_SIGNAL_AUTH_SECRET="+secret,
		"DESKLINK_ALLOW_ANY_ORIGIN=1",
		"DESKLINK_ALLOWED_ORIGINS=",
		"DESKLINK_CONTROLLER_CREDENTIALS_FILE=",
		"DESKLINK_DEVICE_CREDENTIALS_FILE=",
		"DESKLINK_DEVICE_AUTH_SECRET=",
		"DESKLINK_REVOKED_DEVICE_IDS=",
		"DESKLINK_REVOKED_DEVICE_IDS_FILE=",
		"DESKLINK_METRICS_TOKEN=",
	)
	var logs bytes.Buffer
	cmd.Stdout = &logs
	cmd.Stderr = &logs
	if err := cmd.Start(); err != nil {
		t.Fatalf("start signal test server: %v", err)
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

	healthURL := "http://" + address + "/healthz"
	client := &http.Client{Timeout: 250 * time.Millisecond}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		response, err := client.Get(healthURL)
		if err == nil {
			_ = response.Body.Close()
			if response.StatusCode == http.StatusOK {
				return "ws://" + address + "/ws", secret
			}
		}
		time.Sleep(25 * time.Millisecond)
	}

	stop()
	t.Fatalf("signal test server did not become healthy:\n%s", logs.String())
	return "", ""
}

func readSignalE2EMessage(t *testing.T, conn *websocket.Conn, timeout time.Duration) signalE2EMessage {
	t.Helper()
	if err := conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		t.Fatalf("set websocket read deadline: %v", err)
	}
	var message signalE2EMessage
	if err := conn.ReadJSON(&message); err != nil {
		t.Fatalf("read websocket message: %v", err)
	}
	return message
}

func expectSignalE2ERegistered(t *testing.T, conn *websocket.Conn, deviceID string) {
	t.Helper()
	message := readSignalE2EMessage(t, conn, 2*time.Second)
	if message.Type != "registered" || message.DeviceID != deviceID {
		t.Fatalf("expected registered for %s, got %+v", deviceID, message)
	}
}

func dialSignalE2EController(
	t *testing.T,
	wsEndpoint string,
	controllerID string,
	token string,
) *websocket.Conn {
	t.Helper()
	dialer := websocket.Dialer{
		HandshakeTimeout: 2 * time.Second,
		Subprotocols: []string{
			"desklink-v1",
			controllerAuthProtocolPrefix + token,
		},
	}
	endpoint := wsEndpoint + "?deviceId=" + url.QueryEscape(controllerID)
	conn, response, err := dialer.Dial(endpoint, nil)
	if response != nil && response.Body != nil {
		_ = response.Body.Close()
	}
	if err != nil {
		t.Fatalf("dial controller websocket: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	if conn.Subprotocol() != "desklink-v1" {
		t.Fatalf("server must negotiate only desklink-v1, got %q", conn.Subprotocol())
	}
	expectSignalE2ERegistered(t, conn, controllerID)
	return conn
}

func dialSignalE2EHost(
	t *testing.T,
	wsEndpoint string,
	hostID string,
	token string,
) *websocket.Conn {
	t.Helper()
	endpoint := wsEndpoint + "?deviceId=" + url.QueryEscape(hostID) + "&auth=" + url.QueryEscape(token)
	dialer := websocket.Dialer{HandshakeTimeout: 2 * time.Second}
	conn, response, err := dialer.Dial(endpoint, nil)
	if response != nil && response.Body != nil {
		_ = response.Body.Close()
	}
	if err != nil {
		t.Fatalf("dial host websocket: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	expectSignalE2ERegistered(t, conn, hostID)
	return conn
}

func writeSignalE2EAuthRequest(
	t *testing.T,
	controller *websocket.Conn,
	hostID string,
	session string,
) {
	t.Helper()
	if err := controller.WriteJSON(envelope{
		Type:    "auth-request",
		Target:  hostID,
		Session: session,
		Payload: json.RawMessage(`{"version":1}`),
	}); err != nil {
		t.Fatalf("write auth-request: %v", err)
	}
}

func expectSignalE2EPeerOffline(t *testing.T, controller *websocket.Conn, hostID string) {
	t.Helper()
	message := readSignalE2EMessage(t, controller, 2*time.Second)
	if message.Type != "peer-offline" || message.Target != hostID {
		t.Fatalf("expected peer-offline for %s, got %+v", hostID, message)
	}
}

func expectSignalE2EAuthRequest(
	t *testing.T,
	host *websocket.Conn,
	controllerID string,
	session string,
) {
	t.Helper()
	message := readSignalE2EMessage(t, host, 2*time.Second)
	if message.Type != "auth-request" || message.From != controllerID || message.Session != session {
		t.Fatalf("unexpected forwarded auth-request: %+v", message)
	}
	var payload struct {
		Version int `json:"version"`
	}
	if err := json.Unmarshal(message.Payload, &payload); err != nil || payload.Version != 1 {
		t.Fatalf("unexpected auth-request payload %q: %v", message.Payload, err)
	}
}

func expectSignalE2ENoMessage(t *testing.T, conn *websocket.Conn, timeout time.Duration) {
	t.Helper()
	if err := conn.SetReadDeadline(time.Now().Add(timeout)); err != nil {
		t.Fatalf("set websocket read deadline: %v", err)
	}
	var message signalE2EMessage
	err := conn.ReadJSON(&message)
	if err == nil {
		t.Fatalf("expected no websocket message, got %+v", message)
	}
	if !strings.Contains(strings.ToLower(err.Error()), "timeout") {
		t.Fatalf("expected websocket read timeout, got %v", err)
	}
}

func TestSignalWebSocketHostWaitEndToEnd(t *testing.T) {
	wsEndpoint, secret := startSignalE2EServer(t)

	t.Run("offline request is delivered when host registers", func(t *testing.T) {
		controllerID := "controller-e2e-wait"
		hostID := "host-e2e-wait"
		session := "session-e2e-wait"
		controllerToken := mintControllerSignalToken(
			secret,
			controllerID,
			hostID,
			time.Now().Add(5*time.Minute),
		)
		controller := dialSignalE2EController(t, wsEndpoint, controllerID, controllerToken)

		writeSignalE2EAuthRequest(t, controller, hostID, session)
		expectSignalE2EPeerOffline(t, controller, hostID)

		hostToken := mintSignalAuthToken(secret, hostID, time.Now().Add(5*time.Minute))
		host := dialSignalE2EHost(t, wsEndpoint, hostID, hostToken)
		expectSignalE2EAuthRequest(t, host, controllerID, session)
	})

	t.Run("reconnected controller does not inherit old pending request", func(t *testing.T) {
		controllerID := "controller-e2e-reconnect"
		hostID := "host-e2e-reconnect"
		oldSession := "session-e2e-old"
		newSession := "session-e2e-new"
		controllerToken := mintControllerSignalToken(
			secret,
			controllerID,
			hostID,
			time.Now().Add(5*time.Minute),
		)

		oldController := dialSignalE2EController(t, wsEndpoint, controllerID, controllerToken)
		writeSignalE2EAuthRequest(t, oldController, hostID, oldSession)
		expectSignalE2EPeerOffline(t, oldController, hostID)
		_ = oldController.WriteControl(
			websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.CloseNormalClosure, "test reconnect"),
			time.Now().Add(time.Second),
		)
		_ = oldController.Close()

		// Registering a new socket with the same logical controller ID must not
		// inherit pending authentication state from the old connection instance.
		newController := dialSignalE2EController(t, wsEndpoint, controllerID, controllerToken)
		hostToken := mintSignalAuthToken(secret, hostID, time.Now().Add(5*time.Minute))
		host := dialSignalE2EHost(t, wsEndpoint, hostID, hostToken)
		expectSignalE2ENoMessage(t, host, 300*time.Millisecond)

		writeSignalE2EAuthRequest(t, newController, hostID, newSession)
		expectSignalE2EAuthRequest(t, host, controllerID, newSession)
	})

	if t.Failed() {
		t.Logf("signal E2E endpoint: %s", fmt.Sprintf("%s", wsEndpoint))
	}
}
