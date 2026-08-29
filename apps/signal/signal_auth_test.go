package main

import (
	"testing"
	"time"
)

func TestSignalAuthKnownVector(t *testing.T) {
	now := time.Unix(1_700_000_000, 0)
	expiresAt := time.Unix(1_700_003_600, 0)
	const secret = "desklink-signal-test-secret"
	const deviceID = "web-1234abcd"
	const expected = "1700003600.0o3Q6ZbbiDl1k1F6qMLsjGKe8Godrije-SHIf4NB4pQ"

	if token := mintSignalAuthToken(secret, deviceID, expiresAt); token != expected {
		t.Fatalf("unexpected token: %q", token)
	}
	if !validateSignalAuthToken(secret, deviceID, expected, now) {
		t.Fatal("known-good token was rejected")
	}
}

func TestSignalAuthRejectsExpiredTamperedAndOverlongTokens(t *testing.T) {
	const secret = "secret"
	const deviceID = "office-pc"
	now := time.Unix(1_700_000_000, 0)

	expired := mintSignalAuthToken(secret, deviceID, now.Add(-time.Second))
	if validateSignalAuthToken(secret, deviceID, expired, now) {
		t.Fatal("expired token was accepted")
	}

	valid := mintSignalAuthToken(secret, deviceID, now.Add(time.Hour))
	if validateSignalAuthToken(secret, "other-pc", valid, now) {
		t.Fatal("token was accepted for a different device")
	}
	if validateSignalAuthToken(secret, deviceID, valid+"x", now) {
		t.Fatal("tampered token was accepted")
	}

	tooLong := mintSignalAuthToken(secret, deviceID, now.Add(maxSignalAuthLifetime+time.Second))
	if validateSignalAuthToken(secret, deviceID, tooLong, now) {
		t.Fatal("overlong token lifetime was accepted")
	}
}
