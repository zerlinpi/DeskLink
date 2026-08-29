package main

import (
	"bytes"
	"crypto/hmac"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"os"
	"runtime"
	"strconv"
	"strings"
	"time"
)

const (
	controllerRegistryVersion    = 1
	maxControllerRegistryBytes   = 4 << 20
	defaultControllerSessionTTL  = 15 * time.Minute
	maxControllerSessionTTL      = time.Hour
	maxControllerLoginBodyBytes  = 16 << 10
	controllerTokenPrefix        = "ct1"
	controllerAuthProtocolPrefix = "desklink-auth."
)

type controllerRegistryFile struct {
	Version     int                         `json:"version"`
	Controllers map[string]controllerRecord `json:"controllers"`
}

type controllerRecord struct {
	AccessKeySHA256 string   `json:"accessKeySha256"`
	AllowedDevices  []string `json:"allowedDevices"`
}

type loadedControllerRecord struct {
	accessKeySHA256 [sha256.Size]byte
	allowedDevices  map[string]struct{}
}

type controllerSessionRequest struct {
	AccountID      string `json:"accountId"`
	ControllerID   string `json:"controllerId"`
	TargetDeviceID string `json:"targetDeviceId"`
	AccessKey      string `json:"accessKey"`
}

type controllerSessionResponse struct {
	Token     string `json:"token"`
	ExpiresAt int64  `json:"expiresAt"`
}

type signalRegistrationScope struct {
	AllowedTarget string
	Controller    bool
	ExpiresAt     time.Time
}

func controllerRegistryPath() string {
	return strings.TrimSpace(os.Getenv("DESKLINK_CONTROLLER_CREDENTIALS_FILE"))
}

func controllerSessionTTL() time.Duration {
	value := strings.TrimSpace(os.Getenv("DESKLINK_CONTROLLER_SESSION_TTL"))
	if value == "" {
		return defaultControllerSessionTTL
	}
	parsed, err := time.ParseDuration(value)
	if err != nil || parsed <= 0 || parsed > maxControllerSessionTTL {
		return defaultControllerSessionTTL
	}
	return parsed
}

func loadControllerRegistry() (map[string]loadedControllerRecord, error) {
	path := controllerRegistryPath()
	if path == "" {
		return nil, fmt.Errorf("controller credential registry is not configured")
	}

	file, err := os.Open(path)
	if err != nil {
		return nil, fmt.Errorf("open controller credential registry: %w", err)
	}
	defer file.Close()

	info, err := file.Stat()
	if err != nil {
		return nil, fmt.Errorf("stat controller credential registry: %w", err)
	}
	if !info.Mode().IsRegular() {
		return nil, fmt.Errorf("controller credential registry is not a regular file")
	}
	if runtime.GOOS != "windows" && info.Mode().Perm()&0o022 != 0 {
		return nil, fmt.Errorf("controller credential registry must not be group/world writable")
	}

	data, err := io.ReadAll(io.LimitReader(file, maxControllerRegistryBytes+1))
	if err != nil {
		return nil, fmt.Errorf("read controller credential registry: %w", err)
	}
	if len(data) == 0 || len(data) > maxControllerRegistryBytes {
		return nil, fmt.Errorf("controller credential registry has invalid size")
	}

	decoder := json.NewDecoder(bytes.NewReader(data))
	decoder.DisallowUnknownFields()
	var source controllerRegistryFile
	if err := decoder.Decode(&source); err != nil {
		return nil, fmt.Errorf("decode controller credential registry: %w", err)
	}
	if err := ensureJSONEOF(decoder); err != nil {
		return nil, fmt.Errorf("decode controller credential registry: %w", err)
	}
	if source.Version != controllerRegistryVersion {
		return nil, fmt.Errorf("unsupported controller credential registry version %d", source.Version)
	}
	if len(source.Controllers) == 0 {
		return nil, fmt.Errorf("controller credential registry contains no controllers")
	}

	loaded := make(map[string]loadedControllerRecord, len(source.Controllers))
	for accountID, record := range source.Controllers {
		if !validDeviceID(accountID) {
			return nil, fmt.Errorf("invalid controller account ID %q", accountID)
		}
		digest, err := base64.RawURLEncoding.DecodeString(strings.TrimSpace(record.AccessKeySHA256))
		if err != nil || len(digest) != sha256.Size {
			return nil, fmt.Errorf("invalid controller key hash for %q", accountID)
		}
		if len(record.AllowedDevices) == 0 || len(record.AllowedDevices) > 1024 {
			return nil, fmt.Errorf("controller %q must have at least one allowed device", accountID)
		}

		loadedRecord := loadedControllerRecord{allowedDevices: make(map[string]struct{}, len(record.AllowedDevices))}
		copy(loadedRecord.accessKeySHA256[:], digest)
		for _, deviceID := range record.AllowedDevices {
			if !validDeviceID(deviceID) {
				return nil, fmt.Errorf("controller %q has invalid allowed device %q", accountID, deviceID)
			}
			loadedRecord.allowedDevices[deviceID] = struct{}{}
		}
		loaded[accountID] = loadedRecord
	}
	return loaded, nil
}

func validateControllerAccess(
	accountID, accessKey, targetDeviceID string,
) (bool, error) {
	if !validDeviceID(accountID) || !validDeviceID(targetDeviceID) || accessKey == "" || len(accessKey) > 1024 {
		return false, nil
	}
	registry, err := loadControllerRegistry()
	if err != nil {
		return false, err
	}
	record, ok := registry[accountID]
	if !ok {
		return false, nil
	}
	if _, ok := record.allowedDevices[targetDeviceID]; !ok {
		return false, nil
	}
	actual := sha256.Sum256([]byte(accessKey))
	return subtle.ConstantTimeCompare(actual[:], record.accessKeySHA256[:]) == 1, nil
}

func controllerAccessKeySHA256(accessKey string) string {
	digest := sha256.Sum256([]byte(accessKey))
	return base64.RawURLEncoding.EncodeToString(digest[:])
}

func mintControllerSignalToken(
	secret, controllerID, targetDeviceID string,
	expiresAt time.Time,
) string {
	if secret == "" || !validDeviceID(controllerID) || !validDeviceID(targetDeviceID) {
		return ""
	}
	expiry := strconv.FormatInt(expiresAt.Unix(), 10)
	target := base64.RawURLEncoding.EncodeToString([]byte(targetDeviceID))
	mac := hmac.New(sha256.New, []byte(secret))
	_, _ = mac.Write([]byte(
		"DeskLink controller registration\n" + controllerID + "\n" + targetDeviceID + "\n" + expiry,
	))
	signature := base64.RawURLEncoding.EncodeToString(mac.Sum(nil))
	return controllerTokenPrefix + "." + expiry + "." + target + "." + signature
}

func validateControllerSignalToken(
	secret, controllerID, token string,
	now time.Time,
) (string, bool) {
	if secret == "" || !validDeviceID(controllerID) || token == "" {
		return "", false
	}
	parts := strings.Split(token, ".")
	if len(parts) != 4 || parts[0] != controllerTokenPrefix {
		return "", false
	}
	expiresUnix, err := strconv.ParseInt(parts[1], 10, 64)
	if err != nil {
		return "", false
	}
	expiresAt := time.Unix(expiresUnix, 0)
	if !expiresAt.After(now) || expiresAt.Sub(now) > maxControllerSessionTTL {
		return "", false
	}
	targetBytes, err := base64.RawURLEncoding.DecodeString(parts[2])
	if err != nil || len(targetBytes) == 0 || len(targetBytes) > 128 {
		return "", false
	}
	targetDeviceID := string(targetBytes)
	if !validDeviceID(targetDeviceID) {
		return "", false
	}
	expected := mintControllerSignalToken(secret, controllerID, targetDeviceID, expiresAt)
	expectedParts := strings.Split(expected, ".")
	if len(expectedParts) != 4 {
		return "", false
	}
	providedSig, err := base64.RawURLEncoding.DecodeString(parts[3])
	if err != nil {
		return "", false
	}
	expectedSig, err := base64.RawURLEncoding.DecodeString(expectedParts[3])
	if err != nil || !hmac.Equal(providedSig, expectedSig) {
		return "", false
	}
	return targetDeviceID, true
}

func validatedRegistrationTokenExpiry(token string) (time.Time, bool) {
	parts := strings.Split(token, ".")
	if len(parts) == 2 {
		expiresUnix, err := strconv.ParseInt(parts[0], 10, 64)
		if err != nil {
			return time.Time{}, false
		}
		return time.Unix(expiresUnix, 0), true
	}
	if len(parts) == 4 && parts[0] == controllerTokenPrefix {
		expiresUnix, err := strconv.ParseInt(parts[1], 10, 64)
		if err != nil {
			return time.Time{}, false
		}
		return time.Unix(expiresUnix, 0), true
	}
	return time.Time{}, false
}

func registrationScopeForToken(
	secret, peerID, token string,
	now time.Time,
) (signalRegistrationScope, bool) {
	if validateSignalAuthToken(secret, peerID, token, now) {
		expiresAt, ok := validatedRegistrationTokenExpiry(token)
		if !ok {
			return signalRegistrationScope{}, false
		}
		return signalRegistrationScope{ExpiresAt: expiresAt}, true
	}
	if target, ok := validateControllerSignalToken(secret, peerID, token, now); ok {
		expiresAt, expiryOK := validatedRegistrationTokenExpiry(token)
		if !expiryOK {
			return signalRegistrationScope{}, false
		}
		return signalRegistrationScope{
			AllowedTarget: target,
			Controller:    true,
			ExpiresAt:     expiresAt,
		}, true
	}
	return signalRegistrationScope{}, false
}

// websocketRegistrationToken keeps legacy query authentication for native hosts,
// but lets browsers carry their short-lived controller token in the requested
// WebSocket subprotocol header instead. This avoids putting controller tokens in
// URLs that are commonly captured by reverse-proxy access logs.
//
// The server negotiates only the fixed `desklink-v1` protocol. The auth-bearing
// requested protocol is therefore never echoed to the client in the handshake.
func websocketRegistrationToken(r *http.Request) string {
	if token := strings.TrimSpace(r.URL.Query().Get("auth")); token != "" {
		return token
	}
	for _, raw := range strings.Split(r.Header.Get("Sec-WebSocket-Protocol"), ",") {
		protocol := strings.TrimSpace(raw)
		if strings.HasPrefix(protocol, controllerAuthProtocolPrefix) {
			token := strings.TrimPrefix(protocol, controllerAuthProtocolPrefix)
			if token != "" && len(token) <= 2048 {
				return token
			}
		}
	}
	return ""
}

func signalRegistrationScopeForRequest(
	r *http.Request,
	deviceID string,
) (signalRegistrationScope, bool) {
	secret := os.Getenv("DESKLINK_SIGNAL_AUTH_SECRET")
	if secret == "" {
		return signalRegistrationScope{}, true
	}
	return registrationScopeForToken(secret, deviceID, websocketRegistrationToken(r), time.Now())
}

func setControllerCORS(w http.ResponseWriter, r *http.Request) bool {
	origin := r.Header.Get("Origin")
	if origin == "" {
		return true
	}
	if !originAllowed(origin) {
		return false
	}
	w.Header().Set("Access-Control-Allow-Origin", origin)
	w.Header().Set("Vary", "Origin")
	w.Header().Set("Access-Control-Allow-Headers", "Content-Type")
	w.Header().Set("Access-Control-Allow-Methods", "POST, OPTIONS")
	return true
}

func controllerSessionHandler(guard *ipGuard) http.HandlerFunc {
	return func(w http.ResponseWriter, r *http.Request) {
		if controllerRegistryPath() == "" || os.Getenv("DESKLINK_SIGNAL_AUTH_SECRET") == "" {
			http.NotFound(w, r)
			return
		}
		if !setControllerCORS(w, r) {
			http.Error(w, "origin not allowed", http.StatusForbidden)
			return
		}
		if r.Method == http.MethodOptions {
			w.WriteHeader(http.StatusNoContent)
			return
		}
		if r.Method != http.MethodPost {
			w.Header().Set("Allow", "POST, OPTIONS")
			http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
			return
		}

		clientIP := requestClientIP(r)
		now := time.Now()
		if guard != nil && !guard.reserve(clientIP, now) {
			w.Header().Set("Retry-After", "45")
			http.Error(w, "too many authentication attempts", http.StatusTooManyRequests)
			return
		}
		if guard != nil {
			defer guard.release(clientIP, time.Now())
		}

		body := http.MaxBytesReader(w, r.Body, maxControllerLoginBodyBytes)
		decoder := json.NewDecoder(body)
		decoder.DisallowUnknownFields()
		var request controllerSessionRequest
		if err := decoder.Decode(&request); err != nil || ensureJSONEOF(decoder) != nil {
			http.Error(w, "invalid controller session request", http.StatusBadRequest)
			return
		}
		if !validDeviceID(request.AccountID) || !validDeviceID(request.ControllerID) ||
			!validDeviceID(request.TargetDeviceID) || request.AccessKey == "" {
			http.Error(w, "invalid controller session request", http.StatusBadRequest)
			return
		}

		revoked, err := deviceRevoked(request.TargetDeviceID)
		if err != nil {
			http.Error(w, "device authorization unavailable", http.StatusServiceUnavailable)
			return
		}
		if revoked {
			http.Error(w, "target device revoked", http.StatusForbidden)
			return
		}

		allowed, err := validateControllerAccess(
			request.AccountID,
			request.AccessKey,
			request.TargetDeviceID,
		)
		if err != nil {
			http.Error(w, "controller credential registry unavailable", http.StatusServiceUnavailable)
			return
		}
		if !allowed {
			if guard != nil {
				guard.authFailed(clientIP, time.Now())
			}
			http.Error(w, "unauthorized controller", http.StatusUnauthorized)
			return
		}
		if guard != nil {
			guard.authSucceeded(clientIP, time.Now())
		}

		expiresAt := time.Now().Add(controllerSessionTTL())
		token := mintControllerSignalToken(
			os.Getenv("DESKLINK_SIGNAL_AUTH_SECRET"),
			request.ControllerID,
			request.TargetDeviceID,
			expiresAt,
		)
		if token == "" {
			http.Error(w, "unable to issue controller session", http.StatusInternalServerError)
			return
		}

		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cache-Control", "no-store")
		w.Header().Set("Pragma", "no-cache")
		_ = json.NewEncoder(w).Encode(controllerSessionResponse{
			Token:     token,
			ExpiresAt: expiresAt.Unix(),
		})
	}
}
