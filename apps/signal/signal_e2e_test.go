package main

import (
	"bytes"
	"encoding/json"
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

const (
	signalE2EHelperEnv    = "DESKLINK_SIGNAL_E2E_HELPER"
	signalE2EMetricsToken = "desklink-signal-e2e-metrics"
)

type signalE2EServer struct {
	wsEndpoint   string
	httpEndpoint string
	signalSecret string
	metricsToken string
}

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

func startSignalE2EServer(t *testing.T) signalE2EServer {
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
		"DESKLINK_TRUST_PROXY_HEADERS=0",
		"DESKLINK_CONTROLLER_CREDENTIALS_FILE=",
		"DESKLINK_DEVICE_CREDENTIALS_FILE=",
		"DESKLINK_DEVICE_AUTH_SECRET=",
		"DESKLINK_REVOKED_DEVICE_IDS=",
		"DESKLINK_REVOKED_DEVICE_IDS_FILE=",
		"DESKLINK_METRICS_TOKEN="+signalE2EMetricsToken,
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

	httpEndpoint := "http://" + address
	healthURL := httpEndpoint + "/healthz"
	client := &http.Client{Timeout: 250 * time.Millisecond}
	deadline := time.Now().Add(5 * time.Second)
	for time.Now().Before(deadline) {
		response, err := client.Get(healthURL)
		if err == nil {
			_ = response.Body.Close()
			if response.StatusCode == http.StatusOK {
				return signalE2EServer{
					wsEndpoint:   "ws://" + address + "/ws",
					httpEndpoint: httpEndpoint,
					signalSecret: secret,
					metricsToken: signalE2EMetricsToken,
				}
			}
		}
		time.Sleep(25 * time.Millisecond)
	}

	stop()
	t.Fatalf("signal test server did not become healthy:\n%s", logs.String())
	return signalE2EServer{}
}

func fetchSignalE2EMetrics(t *testing.T, server signalE2EServer) signalMetricsSnapshot {
	t.Helper()
	request, err := http.NewRequest(http.MethodGet, server.httpEndpoint+"/metricsz", nil)
	if err != nil {
		t.Fatalf("build metrics request: %v", err)
	}
	request.Header.Set("Authorization", "Bearer "+server.metricsToken)
	client := &http.Client{Timeout: 2 * time.Second}
	response, err := client.Do(request)
	if err != nil {
		t.Fatalf("fetch signal metrics: %v", err)
	}
	defer response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("metrics endpoint returned HTTP %d", response.StatusCode)
	}
	var snapshot signalMetricsSnapshot
	if err := json.NewDecoder(response.Body).Decode(&snapshot); err != nil {
		t.Fatalf("decode signal metrics: %v", err)
	}
	return snapshot
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
	if err != nil {
		if response != nil && response.Body != nil {
			_ = response.Body.Close()
		}
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
	if err != nil {
		if response != nil && response.Body != nil {
			_ = response.Body.Close()
		}
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

func expectSignalE2EPeerOffline(
	t *testing.T,
	controller *websocket.Conn,
	hostID string,
	expectQueued bool,
) {
	t.Helper()
	message := readSignalE2EMessage(t, controller, 2*time.Second)
	if message.Type != "peer-offline" || message.Target != hostID {
		t.Fatalf("expected peer-offline for %s, got %+v", hostID, message)
	}
	if !expectQueued {
		return
	}
	var payload struct {
		AuthQueued  bool  `json:"authQueued"`
		ExpiresInMs int64 `json:"expiresInMs"`
	}
	if err := json.Unmarshal(message.Payload, &payload); err != nil {
		t.Fatalf("decode peer-offline wait payload: %v", err)
	}
	if !payload.AuthQueued || payload.ExpiresInMs != pendingHostAuthTTL.Milliseconds() {
		t.Fatalf("unexpected queued peer-offline payload: %+v", payload)
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

func TestSignalWebSocketHostWaitEndToEnd(t *testing.T) {
	server := startSignalE2EServer(t)

	t.Run("offline request is queued and delivered when host registers", func(t *testing.T) {
		before := fetchSignalE2EMetrics(t, server)
		controllerID := "controller-e2e-wait"
		hostID := "host-e2e-wait"
		session := "session-e2e-wait"
		controllerToken := mintControllerSignalToken(
			server.signalSecret,
			controllerID,
			hostID,
			time.Now().Add(5*time.Minute),
		)
		controller := dialSignalE2EController(t, server.wsEndpoint, controllerID, controllerToken)

		writeSignalE2EAuthRequest(t, controller, hostID, session)
		expectSignalE2EPeerOffline(t, controller, hostID, true)

		hostToken := mintSignalAuthToken(server.signalSecret, hostID, time.Now().Add(5*time.Minute))
		host := dialSignalE2EHost(t, server.wsEndpoint, hostID, hostToken)
		expectSignalE2EAuthRequest(t, host, controllerID, session)

		after := fetchSignalE2EMetrics(t, server)
		if after.PendingHostAuthQueued < before.PendingHostAuthQueued+1 {
			t.Fatalf("queued metric did not advance: before=%+v after=%+v", before, after)
		}
		if after.PendingHostAuthDelivered < before.PendingHostAuthDelivered+1 {
			t.Fatalf("delivered metric did not advance: before=%+v after=%+v", before, after)
		}
		if after.MessagesForwarded < before.MessagesForwarded+1 {
			t.Fatalf("forwarded metric did not advance: before=%+v after=%+v", before, after)
		}
	})

	t.Run("controller target scope blocks queueing unauthorized host", func(t *testing.T) {
		before := fetchSignalE2EMetrics(t, server)
		controllerID := "controller-e2e-scope"
		allowedHostID := "host-e2e-allowed"
		forbiddenHostID := "host-e2e-forbidden"
		controllerToken := mintControllerSignalToken(
			server.signalSecret,
			controllerID,
			allowedHostID,
			time.Now().Add(5*time.Minute),
		)
		controller := dialSignalE2EController(t, server.wsEndpoint, controllerID, controllerToken)
		writeSignalE2EAuthRequest(t, controller, forbiddenHostID, "session-e2e-scope")

		message := readSignalE2EMessage(t, controller, 2*time.Second)
		if message.Type != "error" || !strings.Contains(message.Message, "authorization scope") {
			t.Fatalf("expected target-scope rejection, got %+v", message)
		}
		after := fetchSignalE2EMetrics(t, server)
		if after.PendingHostAuthQueued != before.PendingHostAuthQueued {
			t.Fatalf("unauthorized target must not enter host-wait queue: before=%+v after=%+v", before, after)
		}
		if after.AuthFailures < before.AuthFailures+1 {
			t.Fatalf("target-scope rejection must increment auth failures: before=%+v after=%+v", before, after)
		}
	})

	t.Run("reconnected controller does not inherit old pending request", func(t *testing.T) {
		controllerID := "controller-e2e-reconnect"
		hostID := "host-e2e-reconnect"
		oldSession := "session-e2e-old"
		newSession := "session-e2e-new"
		controllerToken := mintControllerSignalToken(
			server.signalSecret,
			controllerID,
			hostID,
			time.Now().Add(5*time.Minute),
		)

		oldController := dialSignalE2EController(t, server.wsEndpoint, controllerID, controllerToken)
		writeSignalE2EAuthRequest(t, oldController, hostID, oldSession)
		expectSignalE2EPeerOffline(t, oldController, hostID, true)
		_ = oldController.WriteControl(
			websocket.CloseMessage,
			websocket.FormatCloseMessage(websocket.CloseNormalClosure, "test reconnect"),
			time.Now().Add(time.Second),
		)
		_ = oldController.Close()

		// A new socket with the same logical controller ID must not inherit the
		// old socket's pending authentication state. If the old request leaked,
		// it would be the first auth-request delivered to the host below and the
		// new-session assertion would fail.
		newController := dialSignalE2EController(t, server.wsEndpoint, controllerID, controllerToken)
		hostToken := mintSignalAuthToken(server.signalSecret, hostID, time.Now().Add(5*time.Minute))
		host := dialSignalE2EHost(t, server.wsEndpoint, hostID, hostToken)

		writeSignalE2EAuthRequest(t, newController, hostID, newSession)
		expectSignalE2EAuthRequest(t, host, controllerID, newSession)
	})
}
