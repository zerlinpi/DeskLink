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
	"unicode"
)

const registryVersion = 1

type registryFile struct {
	Version     int                         `json:"version"`
	Controllers map[string]controllerRecord `json:"controllers"`
}

type controllerRecord struct {
	AccessKeySHA256 string   `json:"accessKeySha256"`
	AllowedDevices  []string `json:"allowedDevices"`
}

func validID(value string) bool {
	if value == "" || len(value) > 128 {
		return false
	}
	for _, r := range value {
		if unicode.IsLetter(r) || unicode.IsDigit(r) || r == '-' || r == '_' || r == '.' {
			continue
		}
		return false
	}
	return true
}

func loadRegistry(path string) (registryFile, error) {
	registry := registryFile{
		Version:     registryVersion,
		Controllers: map[string]controllerRecord{},
	}
	data, err := os.ReadFile(path)
	if os.IsNotExist(err) {
		return registry, nil
	}
	if err != nil {
		return registryFile{}, err
	}
	decoder := json.NewDecoder(strings.NewReader(string(data)))
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&registry); err != nil {
		return registryFile{}, fmt.Errorf("decode existing registry: %w", err)
	}
	if registry.Version != registryVersion {
		return registryFile{}, fmt.Errorf("unsupported registry version %d", registry.Version)
	}
	if registry.Controllers == nil {
		registry.Controllers = map[string]controllerRecord{}
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

	temp, err := os.CreateTemp(directory, ".desklink-controller-registry-*")
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
	if len(os.Args) < 4 {
		fmt.Fprintln(os.Stderr, "usage: go run set-controller-registry-key.go <registry-file> <account-id> <device-id> [device-id...]")
		os.Exit(2)
	}

	path := strings.TrimSpace(os.Args[1])
	accountID := strings.TrimSpace(os.Args[2])
	if path == "" || !validID(accountID) {
		fmt.Fprintln(os.Stderr, "registry file and a valid account ID are required")
		os.Exit(2)
	}

	seen := map[string]struct{}{}
	allowed := make([]string, 0, len(os.Args)-3)
	for _, raw := range os.Args[3:] {
		deviceID := strings.TrimSpace(raw)
		if !validID(deviceID) {
			fmt.Fprintf(os.Stderr, "invalid device ID: %q\n", raw)
			os.Exit(2)
		}
		if _, exists := seen[deviceID]; exists {
			continue
		}
		seen[deviceID] = struct{}{}
		allowed = append(allowed, deviceID)
	}
	if len(allowed) == 0 {
		fmt.Fprintln(os.Stderr, "at least one allowed device is required")
		os.Exit(2)
	}

	randomBytes := make([]byte, 32)
	if _, err := rand.Read(randomBytes); err != nil {
		fmt.Fprintln(os.Stderr, "generate controller key:", err)
		os.Exit(1)
	}
	accessKey := "ck1." + base64.RawURLEncoding.EncodeToString(randomBytes)
	digest := sha256.Sum256([]byte(accessKey))

	registry, err := loadRegistry(path)
	if err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}
	registry.Controllers[accountID] = controllerRecord{
		AccessKeySHA256: base64.RawURLEncoding.EncodeToString(digest[:]),
		AllowedDevices:  allowed,
	}
	if err := writeRegistryAtomic(path, registry); err != nil {
		fmt.Fprintln(os.Stderr, err)
		os.Exit(1)
	}

	// Print only the new controller key. The server registry stores only its hash.
	fmt.Println(accessKey)
}
