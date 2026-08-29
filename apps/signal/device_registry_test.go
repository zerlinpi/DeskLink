package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"os"
	"path/filepath"
	"runtime"
	"testing"
)

func writeDeviceRegistry(
	t *testing.T,
	entries map[string]string,
) string {
	t.Helper()
	devices := make(map[string]deviceCredentialRecord, len(entries))
	for deviceID, credential := range entries {
		devices[deviceID] = deviceCredentialRecord{
			CredentialSHA256: credentialSHA256String(credential),
		}
	}
	data, err := json.Marshal(deviceCredentialRegistryFile{
		Version: deviceCredentialRegistryVersion,
		Devices: devices,
	})
	if err != nil {
		t.Fatalf("marshal registry: %v", err)
	}
	path := filepath.Join(t.TempDir(), "devices.json")
	if err := os.WriteFile(path, data, 0o600); err != nil {
		t.Fatalf("write registry: %v", err)
	}
	return path
}

func TestDeviceRegistryValidatesIndependentCredential(t *testing.T) {
	const credentialA = "dc2.host-a-independent-random-value"
	const credentialB = "dc2.host-b-independent-random-value"
	path := writeDeviceRegistry(t, map[string]string{
		"host-a": credentialA,
		"host-b": credentialB,
	})
	t.Setenv("DESKLINK_DEVICE_CREDENTIALS_FILE", path)

	ok, err := validateDeviceCredentialSource("legacy-secret", "host-a", credentialA)
	if err != nil || !ok {
		t.Fatalf("expected host-a registry credential to validate, ok=%v err=%v", ok, err)
	}
	ok, err = validateDeviceCredentialSource("legacy-secret", "host-a", credentialB)
	if err != nil {
		t.Fatalf("unexpected validation error: %v", err)
	}
	if ok {
		t.Fatal("credential for host-b must not validate for host-a")
	}
}

func TestConfiguredRegistryDoesNotFallBackToLegacyMasterSecret(t *testing.T) {
	const deviceID = "host-a"
	const legacySecret = "legacy-master-secret"
	path := writeDeviceRegistry(t, map[string]string{
		deviceID: "dc2.new-independent-credential",
	})
	t.Setenv("DESKLINK_DEVICE_CREDENTIALS_FILE", path)

	legacyCredential := deriveDeviceCredential(legacySecret, deviceID)
	ok, err := validateDeviceCredentialSource(legacySecret, deviceID, legacyCredential)
	if err != nil {
		t.Fatalf("unexpected validation error: %v", err)
	}
	if ok {
		t.Fatal("configured registry must not silently fall back to legacy HMAC credentials")
	}
}

func TestDeviceRegistryFailureIsFailClosed(t *testing.T) {
	path := filepath.Join(t.TempDir(), "broken.json")
	if err := os.WriteFile(path, []byte("{not-json"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("DESKLINK_DEVICE_CREDENTIALS_FILE", path)

	if _, err := validateDeviceCredentialSource("legacy-secret", "host-a", "anything"); err == nil {
		t.Fatal("expected malformed registry to fail closed")
	}
}

func TestDeviceRegistryRejectsWritableByUnprivilegedUsers(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("POSIX mode bits are not authoritative on Windows")
	}
	path := writeDeviceRegistry(t, map[string]string{
		"host-a": "dc2.independent",
	})
	if err := os.Chmod(path, 0o666); err != nil {
		t.Fatal(err)
	}
	t.Setenv("DESKLINK_DEVICE_CREDENTIALS_FILE", path)
	if _, err := loadDeviceCredentialRegistry(); err == nil {
		t.Fatal("expected group/world-writable registry to be rejected")
	}
}

func TestSignalTokenHandlerAcceptsRegistryWithoutLegacyDeviceSecret(t *testing.T) {
	const deviceID = "host-a"
	const credential = "dc2.registry-only-independent-credential"
	path := writeDeviceRegistry(t, map[string]string{deviceID: credential})
	t.Setenv("DESKLINK_DEVICE_CREDENTIALS_FILE", path)
	t.Setenv("DESKLINK_DEVICE_AUTH_SECRET", "")
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "signal-secret")

	req := httptest.NewRequest(http.MethodGet, "/api/v1/signal-token?deviceId="+deviceID, nil)
	req.Header.Set("Authorization", "Bearer "+credential)
	rec := httptest.NewRecorder()
	signalTokenHandler()(rec, req)
	if rec.Code != http.StatusOK {
		t.Fatalf("expected 200 with registry-only auth, got %d: %s", rec.Code, rec.Body.String())
	}
}

func TestSignalTokenHandlerReturnsUnavailableForBrokenRegistry(t *testing.T) {
	path := filepath.Join(t.TempDir(), "broken.json")
	if err := os.WriteFile(path, []byte("{}"), 0o600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("DESKLINK_DEVICE_CREDENTIALS_FILE", path)
	t.Setenv("DESKLINK_DEVICE_AUTH_SECRET", "legacy-secret")
	t.Setenv("DESKLINK_SIGNAL_AUTH_SECRET", "signal-secret")

	req := httptest.NewRequest(http.MethodGet, "/api/v1/signal-token?deviceId=host-a", nil)
	req.Header.Set("Authorization", "Bearer anything")
	rec := httptest.NewRecorder()
	signalTokenHandler()(rec, req)
	if rec.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503 for broken configured registry, got %d", rec.Code)
	}
}
