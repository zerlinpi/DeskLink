package main

import (
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
)

func clearReadinessEnvironment(t *testing.T) {
	t.Helper()
	for _, name := range []string{
		"DESKLINK_DEVICE_CREDENTIALS_FILE",
		"DESKLINK_CONTROLLER_CREDENTIALS_FILE",
		"DESKLINK_REVOKED_DEVICE_IDS",
		"DESKLINK_REVOKED_DEVICE_IDS_FILE",
		"DESKLINK_SIGNAL_AUTH_SECRET",
		"DESKLINK_TURN_AUTH_SECRET",
		"DESKLINK_REQUIRE_AUTH_READY",
		"DESKLINK_REQUIRE_TURN_READY",
	} {
		t.Setenv(name, "")
	}
}

func TestReadinessAllowsDevelopmentModeWithoutProductionAuth(t *testing.T) {
	clearReadinessEnvironment(t)
	if failures := readinessFailures(); len(failures) != 0 {
		t.Fatalf("development readiness unexpectedly failed: %v", failures)
	}

	request := httptest.NewRequest(http.MethodGet, "/readyz", nil)
	response := httptest.NewRecorder()
	readinessHandler(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("expected 200, got %d: %s", response.Code, response.Body.String())
	}
}

func TestReadinessFailsClosedForMissingConfiguredRegistry(t *testing.T) {
	clearReadinessEnvironment(t)
	t.Setenv("DESKLINK_DEVICE_CREDENTIALS_FILE", filepath.Join(t.TempDir(), "missing-devices.json"))
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "test-signal-secret")

	failures := readinessFailures()
	if !containsReadinessFailure(failures, "device-registry") {
		t.Fatalf("expected device-registry failure, got %v", failures)
	}
}

func TestReadinessRequiresProductionSecrets(t *testing.T) {
	clearReadinessEnvironment(t)
	t.Setenv("DESKLINK_REQUIRE_AUTH_READY", "1")
	t.Setenv("DESKLINK_REQUIRE_TURN_READY", "1")

	failures := readinessFailures()
	for _, expected := range []string{
		"signal-auth-secret",
		"device-registry-not-configured",
		"controller-registry-not-configured",
		"turn-auth-secret",
	} {
		if !containsReadinessFailure(failures, expected) {
			t.Fatalf("expected %q failure, got %v", expected, failures)
		}
	}

	request := httptest.NewRequest(http.MethodGet, "/readyz", nil)
	response := httptest.NewRecorder()
	readinessHandler(response, request)
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503, got %d: %s", response.Code, response.Body.String())
	}
	if !strings.Contains(response.Body.String(), `"ok":false`) {
		t.Fatalf("expected failed readiness JSON, got %s", response.Body.String())
	}
}

func containsReadinessFailure(failures []string, wanted string) bool {
	for _, failure := range failures {
		if failure == wanted {
			return true
		}
	}
	return false
}
