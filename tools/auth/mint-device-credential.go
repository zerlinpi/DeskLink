package main

import (
	"crypto/hmac"
	"crypto/sha256"
	"encoding/base64"
	"fmt"
	"os"
)

func validDeviceID(id string) bool {
	if id == "" || len(id) > 128 {
		return false
	}
	for _, r := range id {
		if (r >= 'a' && r <= 'z') ||
			(r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') ||
			r == '-' || r == '_' || r == '.' {
			continue
		}
		return false
	}
	return true
}

func main() {
	if len(os.Args) != 2 || !validDeviceID(os.Args[1]) {
		fmt.Fprintln(os.Stderr, "usage: go run tools/auth/mint-device-credential.go <device-id>")
		os.Exit(2)
	}

	secret := os.Getenv("DESKLINK_DEVICE_AUTH_SECRET")
	if secret == "" {
		fmt.Fprintln(os.Stderr, "DESKLINK_DEVICE_AUTH_SECRET is required")
		os.Exit(2)
	}

	deviceID := os.Args[1]
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte("DeskLink device credential\n" + deviceID))
	credential := "dc1." + base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	fmt.Println(credential)
}
