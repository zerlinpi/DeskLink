package main

import (
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

const registryVersion = 1

type registryFile struct {
	Version int                       `json:"version"`
	Devices map[string]registryRecord `json:"devices"`
}

type registryRecord struct {
	CredentialSHA256 string `json:"credentialSha256"`
}

func validDeviceID(value string) bool {
	if value == "" || len(value) > 128 {
		return false
	}
	for _, r := range value {
		if (r >= 'a' && r <= 'z') || (r >= 'A' && r <= 'Z') ||
			(r >= '0' && r <= '9') || r == '-' || r == '_' || r == '.' {
			continue
		}
		return false
	}
	return true
}

func loadRegistry(path string) (registryFile, error) {
	registry := registryFile{
		Version: registryVersion,
		Devices: map[string]registryRecord{},
	}
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return registry, nil
	}
	if err != nil {
		return registryFile{}, err
	}
	if err := json.Unmarshal(data, &registry); err != nil {
		return registryFile{}, fmt.Errorf("decode existing registry: %w", err)
	}
	if registry.Version != registryVersion {
		return registryFile{}, fmt.Errorf("unsupported registry version %d", registry.Version)
	}
	if registry.Devices == nil {
		registry.Devices = map[string]registryRecord{}
	}
	return registry, nil
}

func writeRegistryAtomic(path string, registry registryFile) error {
	directory := filepath.Dir(path)
	if directory == "" {
		directory = "."
	}
	if err := os.MkdirAll(directory, 0o700); err != nil {
		return fmt.Errorf("create registry directory: %w", err)
	}

	temp, err := os.CreateTemp(directory, ".desklink-device-registry-*")
	if err != nil {
		return fmt.Errorf("create registry temp file: %w", err)
	}
	tempPath := temp.Name()
	committed := false
	defer func() {
		_ = temp.Close()
		if !committed {
			_ = os.Remove(tempPath)
		}
	}()

	if err := temp.Chmod(0o600); err != nil {
		return fmt.Errorf("protect registry temp file: %w", err)
	}
	encoder := json.NewEncoder(temp)
	encoder.SetIndent("", "  ")
	if err := encoder.Encode(registry); err != nil {
		return fmt.Errorf("encode registry: %w", err)
	}
	if err := temp.Sync(); err != nil {
		return fmt.Errorf("sync registry temp file: %w", err)
	}
	if err := temp.Close(); err != nil {
		return fmt.Errorf("close registry temp file: %w", err)
	}

	// On Unix the rename is atomic and replaces the previous regular file. The
	// signaling service reads the file for every credential check, so the new
	// credential becomes active without a process restart.
	if err := os.Rename(tempPath, path); err != nil {
		return fmt.Errorf("install registry: %w", err)
	}
	if err := os.Chmod(path, 0o600); err != nil {
		return fmt.Errorf("protect registry: %w", err)
	}
	committed = true
	return nil
}

func main() {
	if len(os.Args) != 3 {
		fmt.Fprintln(os.Stderr, "usage: go run rotate-device-registry-credential.go <registry-file> <device-id>")
		os.Exit(2)
	}
	path := strings.TrimSpace(os.Args[1])
	deviceID := strings.TrimSpace(os.Args[2])
	if path == "" || !validDeviceID(deviceID) {
		fmt.Fprintln(os.Stderr, "registry file and a valid device ID are required")
		os.Exit(2)
	}

	randomBytes := make([]byte, 32)
	if _, err := rand.Read(randomBytes); err != nil {
		fmt.Fprintln(os.Stderr, "generate credential:", err)
		os.Exit(1)
	}
	credential := "dc2." + base64.RawURLEncoding.EncodeToString(randomBytes)
	digest := sha256.Sum256([]byte(credential))
	hash := base64.RawURLEncoding.EncodeToString(digest[:])

	registry, err := loadRegistry(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	registry.Devices[deviceID] = registryRecord{CredentialSHA256: hash}
	if err := writeRegistryAtomic(path, registry); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	// stdout intentionally contains only the new host credential so callers can
	// redirect/copy it without exposing the registry or any server master secret.
	fmt.Println(credential)
}
