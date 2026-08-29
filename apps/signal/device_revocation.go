package main

import (
	"errors"
	"os"
	"strings"
)

const maxRevocationFileBytes = 1 << 20

func parseRevokedDeviceIDs(value string) map[string]struct{} {
	result := make(map[string]struct{})
	for _, item := range strings.FieldsFunc(value, func(r rune) bool {
		switch r {
		case ',', ';', '\n', '\r', '\t', ' ':
			return true
		default:
			return false
		}
	}) {
		id := strings.TrimSpace(item)
		if validDeviceID(id) {
			result[id] = struct{}{}
		}
	}
	return result
}

func loadRevokedDeviceIDs() (map[string]struct{}, error) {
	result := parseRevokedDeviceIDs(os.Getenv("DESKLINK_REVOKED_DEVICE_IDS"))
	path := strings.TrimSpace(os.Getenv("DESKLINK_REVOKED_DEVICE_IDS_FILE"))
	if path == "" {
		return result, nil
	}

	info, err := os.Stat(path)
	if err != nil {
		return nil, err
	}
	if info.Size() < 0 || info.Size() > maxRevocationFileBytes {
		return nil, errors.New("device revocation file is too large")
	}

	data, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	for id := range parseRevokedDeviceIDs(string(data)) {
		result[id] = struct{}{}
	}
	return result, nil
}

// Authentication and credential endpoints call this directly so newly revoked
// devices fail closed immediately. Long-lived WebSocket peers use the shared
// revocation sweep in main.go, which reads the same source once per sweep rather
// than once per connection.
func deviceRevoked(deviceID string) (bool, error) {
	if !validDeviceID(deviceID) {
		return false, errors.New("valid device ID is required")
	}

	revokedDevices, err := loadRevokedDeviceIDs()
	if err != nil {
		return false, err
	}
	_, revoked := revokedDevices[deviceID]
	return revoked, nil
}
