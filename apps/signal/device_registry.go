package main

import (
	"bytes"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"os"
	"runtime"
	"strings"
)

const (
	deviceCredentialRegistryVersion = 1
	maxDeviceRegistryBytes          = 4 << 20
)

type deviceCredentialRegistryFile struct {
	Version int                               `json:"version"`
	Devices map[string]deviceCredentialRecord `json:"devices"`
}

type deviceCredentialRecord struct {
	CredentialSHA256 string `json:"credentialSha256"`
}

type loadedDeviceCredentialRegistry map[string][sha256.Size]byte

func deviceCredentialRegistryPath() string {
	return strings.TrimSpace(os.Getenv("DESKLINK_DEVICE_CREDENTIALS_FILE"))
}

func deviceCredentialAuthConfigured(legacySecret string) bool {
	return deviceCredentialRegistryPath() != "" || legacySecret != ""
}

func credentialSHA256String(credential string) string {
	digest := sha256.Sum256([]byte(credential))
	return base64.RawURLEncoding.EncodeToString(digest[:])
}

func loadDeviceCredentialRegistry() (loadedDeviceCredentialRegistry, error) {
	path := deviceCredentialRegistryPath()
	if path == "" {
		return nil, fmt.Errorf("device credential registry is not configured")
	}

	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open device credential registry: %w", err)
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return nil, fmt.Errorf("stat device credential registry: %w", err)
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("device credential registry is not a regular file")
	}
	// On Unix-like production hosts, refuse a registry that unprivileged users
	// can modify. Read-only group/world access is acceptable for container secret
	// mounts; write access is not.
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o022 != 0 {
		return nil, fmt.Errorf("device credential registry must not be group/world writable")
	}

	data, err := io.ReadAll(io.LimitReader(file, maxDeviceRegistryBytes+1))
	if err != nil {
		return nil, fmt.Errorf("read device credential registry: %w", err)
	}
	if len(data) == 0 || len(data) > maxDeviceRegistryBytes {
		return nil, fmt.Errorf("device credential registry has invalid size")
	}

	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	var source deviceCredentialRegistryFile
	if err := decoder.Decode(&source); err != nil {
		return nil, fmt.Errorf("decode device credential registry: %w", err)
	}
	if err := ensureJSONEOF(decoder); err != nil {
		return nil, fmt.Errorf("decode device credential registry: %w", err)
	}
	if source.Version != deviceCredentialRegistryVersion {
		return nil, fmt.Errorf("unsupported device credential registry version %d", source.Version)
	}
	if len(source.Devices) == 0 {
		return nil, fmt.Errorf("device credential registry contains no devices")
	}

	loaded := make(loadedDeviceCredentialRegistry, len(source.Devices))
	for deviceID, record := range source.Devices {
		if !validDeviceID(deviceID) {
			return nil, fmt.Errorf("device credential registry contains invalid device ID %q", deviceID)
		}
		digest, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(record.CredentialSHA256))
		if err != nil || len(digest) != sha256.Size {
			return nil, fmt.Errorf("device credential registry contains invalid hash for %q", deviceID)
		}
		var fixed [sha256.Size]byte
		copy(fixed[:], digest)
		loaded[deviceID] = fixed
	}
	return loaded, nil
}

func ensureJSONEOF(decoder *json.Decoder) error {
	var extra any
	if err := decoder.Decode(&extra); err != io.EOF {
		if err == nil {
			return fmt.Errorf("multiple JSON values are not allowed")
		}
		return err
	}
	return nil
}

// validateDeviceCredentialSource prefers the independently rotatable registry
// whenever DESKLINK_DEVICE_CREDENTIALS_FILE is configured. The legacy HMAC
// master-secret mode is used only when no registry path is configured, making
// migration explicit and preventing a missing registry entry from silently
// falling back to a deterministic credential.
func validateDeviceCredentialSource(
	legacySecret, deviceID, credential string,
) (bool, error) {
	if deviceCredentialRegistryPath() == "" {
		return validateDeviceCredential(legacySecret, deviceID, credential), nil
	}

	registry, err := loadDeviceCredentialRegistry()
	if err != nil {
		return false, err
	}
	expected, ok := registry[deviceID]
	if !ok || credential == "" {
		return false, nil
	}
	actual := sha256.Sum256([]byte(credential))
	return subtle.ConstantTimeCompare(actual[:], expected[:]) == 1, nil
}
