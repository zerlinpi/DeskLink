package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func writeControllerRegistry(
	t *testing.T,
	accountID, accessKey string,
	allowedDevices []string,
) string {
	t.Helper()
	data, err := json.Marshal(controllerRegistryFile{
		Version: controllerRegistryVersion,
		Controllers: map[string]controllerRecord{
			accountID: {
				AccessKeySHA256: controllerAccessKeySHA256(accessKey),
				AllowedDevices:  allowedDevices,
			},
		},
	})
	if err != nil {
		t.Fatalf("marshal controller registry: %v", err)
	}
	path := filepath.Join(t.TempDir(), "controllers.json")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatalf("write controller registry: %v", err)
	}
	return path
}

func TestControllerAccessIsBoundToAccountKeyAndTarget(t *testing.T) {
	path := writeControllerRegistry(
		t,
		"alice",
		"ck1.alice-high-entropy-key",
		[]string{"office-pc"},
	)
	t.Setenv("DESKLINK_CONTROLLER_CREDENTIALS_FILE", path)

	ok, err := validateControllerAccess("alice", "ck1.alice-high-entropy-key", "office-pc")
	if err != nil || !ok {
		t.Fatalf("expected controller access, ok=%v err=%v", ok, err)
	}
	for _, tc := range []struct {
		account string
		key     string
		target  string
	}{
		{"alice", "wrong", "office-pc"},
		{"bob", "ck1.alice-high-entropy-key", "office-pc"},
		{"alice", "ck1.alice-high-entropy-key", "other-pc"},
	} {
		ok, err := validateControllerAccess(tc.account, tc.key, tc.target)
		if err != nil {
			t.Fatalf("unexpected validation error: %v", err)
		}
		if ok {
			t.Fatalf("unexpected controller authorization for %+v", tc)
		}
	}
}

func TestControllerSignalTokenIsBoundToPeerAndTarget(t *testing.T) {
	const secret = "controller-signal-secret"
	now := time.Unix(1_800_000_000, 0)
	expiresAt := now.Add(15 * time.Minute)
	token := mintControllerSignalToken(secret, "web-abc12345", "office-pc", expiresAt)
	if token == "" {
		t.Fatal("expected controller token")
	}

	target, ok := validateControllerSignalToken(secret, "web-abc12345", token, now)
	if !ok || target != "office-pc" {
		t.Fatalf("unexpected controller token validation: target=%q ok=%v", target, ok)
	}
	if _, ok := validateControllerSignalToken(secret, "web-other", token, now); ok {
		t.Fatal("controller token must not validate for another browser peer")
	}
	if _, ok := validateControllerSignalToken(secret, "web-abc12345", token+"x", now); ok {
		t.Fatal("tampered controller token was accepted")
	}
	if _, ok := validateControllerSignalToken(secret, "web-abc12345", token, expiresAt); ok {
		t.Fatal("expired controller token was accepted")
	}
}

func TestControllerSessionHandlerIssuesTargetScopedToken(t *testing.T) {
	const signalSecret = "signal-secret"
	path := writeControllerRegistry(t, "alice", "ck1.good-key", []string{"office-pc"})
	t.Setenv("DESKLINK_CONTROLLER_CREDENTIALS_FILE", path)
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", signalSecret)
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", "")

	body := `{"accountId":"alice","controllerId":"web-1234abcd","targetDeviceId":"office-pc","accessKey":"ck1.good-key"}`
	req := httptest.NewRequest(http.MethodPost, "/api/v1/controller-session", stringsNewReader(body))
	req.RemoteAddr = "127.0.0.1:12345"
	rec := httptest.NewRecorder()
	controllerSessionHandler(newIPGuard())(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", rec.Code, rec.Body.String())
	}

	var response controllerSessionResponse
	if err := json.Unmarshal(rec.Body.Bytes(), &response); err != nil {
		t.Fatalf("decode session response: %v", err)
	}
	target, ok := validateControllerSignalToken(
		signalSecret,
		"web-1234abcd",
		response.Token,
		time.Now(),
	)
	if !ok || target != "office-pc" {
		t.Fatalf("issued token scope invalid: target=%q ok=%v", target, ok)
	}
	if rec.Header().Get("Cache-Control") != "no-store" {
		t.Fatal("controller session response must be no-store")
	}
}

func TestControllerSessionRejectsUnauthorizedAndRevokedTarget(t *testing.T) {
	path := writeControllerRegistry(t, "alice", "ck1.good-key", []string{"office-pc"})
	t.Setenv("DESKLINK_CONTROLLER_CREDENTIALS_FILE", path)
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "signal-secret")

	request := func(accessKey string) *httptest.ResponseRecorder {
		body := `{"accountId":"alice","controllerId":"web-1234abcd","targetDeviceId":"office-pc","accessKey":"` + accessKey + `"}`
		req := httptest.NewRequest(http.MethodPost, "/api/v1/controller-session", stringsNewReader(body))
		req.RemoteAddr = "127.0.0.1:23456"
		rec := httptest.NewRecorder()
		controllerSessionHandler(newIPGuard())(rec, req)
		return rec
	}

	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", "")
	if rec := request("wrong-key"); rec.Code != http.StatusUnauthorized {
		t.Fatalf("expected 401 for wrong key, got %d", rec.Code)
	}

	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", "office-pc")
	if rec := request("ck1.good-key"); rec.Code != http.StatusForbidden {
		t.Fatalf("expected 403 for revoked target, got %d", rec.Code)
	}
}

func TestControllerPeerCannotSignalOutsideScope(t *testing.T) {
	p := newPeer("web-1234abcd", nil, signalRegistrationScope{
		AllowedTarget: "office-pc",
		Controller:    true,
	})
	if !p.canSignalTarget("office-pc") {
		t.Fatal("authorized target was rejected")
	}
	if p.canSignalTarget("other-pc") {
		t.Fatal("controller peer was allowed to signal outside its token scope")
	}
}

// Kept local so the test file does not need to expose any body helper to the
// application package.
func stringsNewReader(value string) *strings.Reader {
	return strings.NewReader(value)
}
