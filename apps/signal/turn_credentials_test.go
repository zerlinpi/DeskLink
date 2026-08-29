package main

import (
	"testing"
	"time"
)

func TestMintTurnCredentialsKnownVector(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	credentials, err := mintTurnCredentials(
		"desklink-test-secret",
		"device-42",
		time.Hour,
		now,
	)
	if err != nil {
		t.Fatalf("mintTurnCredentials returned error: %v", err)
	}

	if credentials.Username != "1700003600:device-42" {
		t.Fatalf("unexpected username: %q", credentials.Username)
	}
	if credentials.Password != "2/Qtt+lakig8z3wefn9XAscwwqI=" {
		t.Fatalf("unexpected password: %q", credentials.Password)
	}
	if credentials.ExpiresAt != 1_700_003_600 {
		t.Fatalf("unexpected expiry: %d", credentials.ExpiresAt)
	}
}

func TestMintTurnCredentialsRejectsInvalidInput(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)

	tests := []struct {
		name   string
		secret string
		userID string
		ttl    time.Duration
	}{
		{name: "missing secret", userID: "device-42", ttl: time.Hour},
		{name: "invalid user", secret: "secret", userID: "bad:user", ttl: time.Hour},
		{name: "zero ttl", secret: "secret", userID: "device-42", ttl: 0},
		{name: "ttl too long", secret: "secret", userID: "device-42", ttl: 24*time.Hour + time.Second},
	}

	for _, tc := range tests {
		t.Run(tc.name, func(t *testing.T) {
			if _, err := mintTurnCredentials(tc.secret, tc.userID, tc.ttl, now); err == nil {
				t.Fatal("expected error")
			}
		})
	}
}
