package main

import (
	"crypto/hmac"
	"crypto/sha1"
	"encoding/base64"
	"errors"
	"strconv"
	"time"
)

const maxTurnCredentialTTL = 24 * time.Hour

type turnCredentials struct {
	Username  string `json:"username"`
	Password  string `json:"password"`
	ExpiresAt int64  `json:"expiresAt"`
}

// mintTurnCredentials implements the TURN REST temporary credential scheme
// supported by coturn's use-auth-secret/static-auth-secret mode. The HTTP layer
// exposes these credentials only after a valid short-lived signaling token for
// the same device ID has been presented; the coturn shared secret never leaves
// the server.
func mintTurnCredentials(
	secret string,
	userID string,
	ttl time.Duration,
	now time.Time,
) (turnCredentials, error) {
	if secret == "" {
		return turnCredentials{}, errors.New("TURN shared secret is required")
	}
	if !validDeviceID(userID) {
		return turnCredentials{}, errors.New("valid TURN user/device ID is required")
	}
	if ttl <= 0 || ttl > maxTurnCredentialTTL {
		return turnCredentials{}, errors.New("TURN credential TTL must be between 1ns and 24h")
	}

	expiresAt := now.Add(ttl).Unix()
	username := strconv.FormatInt(expiresAt, 10) + ":" + userID

	mac := hmac.New(sha1.New, []byte(secret))
	_, _ = mac.Write([]byte(username))
	password := base64.StdEncoding.EncodeToString(mac.Sum(nil))

	return turnCredentials{
		Username:  username,
		Password:  password,
		ExpiresAt: expiresAt,
	}, nil
}
