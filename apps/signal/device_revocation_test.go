package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestParseRevokedDeviceIDs(t *testing.T) {
	ids := parseRevokedDeviceIDs(" alpha,bravo\ncharlie ; invalid/id  delta ")
	for _, id := range []string{"alpha", "bravo", "charlie", "delta"} {
		if _, ok := ids[id]; !ok {
			t.Fatalf("expected %q to be parsed", id)
		}
	}
	if _, ok := ids["invalid/id"]; ok {
		t.Fatal("invalid device ID must not be accepted")
	}
}

func TestDeviceRevokedInlineAndFile(t *testing.T) {
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", "inline-one,inline-two")
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS_FILE", "")

	revoked, err := deviceRevoked("inline-two")
	if err != nil || !revoked {
		t.Fatalf("expected inline revocation, revoked=%v err=%v", revoked, err)
	}

	dir := t.TempDir()
	path := filepath.Join(dir, "revoked.txt")
	if err := os.WriteFile(path, []byte("file-one\nfile-two\n"), 0600); err != nil {
		t.Fatal(err)
	}
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", "")
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS_FILE", path)

	revoked, err = deviceRevoked("file-one")
	if err != nil || !revoked {
		t.Fatalf("expected file revocation, revoked=%v err=%v", revoked, err)
	}
	revoked, err = deviceRevoked("not-revoked")
	if err != nil || revoked {
		t.Fatalf("unexpected revocation, revoked=%v err=%v", revoked, err)
	}
}

func TestDeviceRevocationFileFailureIsReported(t *testing.T) {
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS", "")
	t.Setenv("DESKLINK_REVOKED_DEVICE_IDS_FILE", filepath.Join(t.TempDir(), "missing.txt"))

	if _, err := deviceRevoked("device-01"); err == nil {
		t.Fatal("configured unreadable revocation file must report an error")
	}
}
