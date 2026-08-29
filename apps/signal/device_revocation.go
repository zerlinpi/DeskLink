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

// deviceRevoked checks both the inline list and an optional administrator-owned
// file. The file is intentionally read on each authentication/credential request:
// these operations are low-frequency, and immediate revocation is more important
// than maintaining a cache that can become stale.
func deviceRevoked(deviceID string) (bool, error) {
	if !validDeviceID(deviceID) {
		return false, errors.New("valid device ID is required")
	}

	if _, ok := parseRevokedDeviceIDs(os.Getenv("DESKLINK_REVOKED_DEVICE_IDS"))[deviceID]; ok {
		return true, nil
	}

	path := strings.TrimSpace(os.Getenv("DESKLINK_REVOKED_DEVICE_IDS_FILE"))
	if path == "" {
		return false, nil
	}

	info, err := os.Stat(path)
	if err != nil {
		return false, err
	}
	if info.Size() < 0 || info.Size() > maxRevocationFileBytes {
		return false, errors.New("device revocation file is too large")
	}

	data, err := os.ReadFile(path)
	if err != nil {
		return false, err
	}
	_, revoked := parseRevokedDeviceIDs(string(data))[deviceID]
	return revoked, nil
}
