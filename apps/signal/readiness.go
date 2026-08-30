package main

import (
	"encoding/json"
	"log"
	"net/http"
	"os"
	"strings"
)

type readinessResponse struct {
	OK     bool     `json:"ok"`
	Failed []string `json:"failed,omitempty"`
}

func readinessFailures() []string {
	failures := make([]string, 0, 5)
	deviceRegistryConfigured := deviceCredentialRegistryPath() != ""
	controllerRegistryConfigured := controllerRegistryPath() != ""

	if deviceRegistryConfigured {
		if _, err := loadDeviceCredentialRegistry(); err != nil {
			log.Printf("readiness: device credential registry unavailable: %v", err)
			failures = append(failures, "device-registry")
		}
	}
	if controllerRegistryConfigured {
		if _, err := loadControllerRegistry(); err != nil {
			log.Printf("readiness: controller credential registry unavailable: %v", err)
			failures = append(failures, "controller-registry")
		}
	}
	if _, err := loadRevokedDeviceIDs(); err != nil {
		log.Printf("readiness: device revocation state unavailable: %v", err)
		failures = append(failures, "device-revocation")
	}

	if (deviceRegistryConfigured || controllerRegistryConfigured ||
		strings.TrimSpace(os.Getenv("DESKLINK_REQUIRE_AUTH_READY")) == "1") &&
		strings.TrimSpace(os.Getenv("DESKLINK_SIGNAL_AUTH_SECRET")) == "" {
		failures = append(failures, "signal-auth-secret")
	}

	if strings.TrimSpace(os.Getenv("DESKLINK_REQUIRE_AUTH_READY")) == "1" {
		if !deviceRegistryConfigured {
			failures = append(failures, "device-registry-not-configured")
		}
		if !controllerRegistryConfigured {
			failures = append(failures, "controller-registry-not-configured")
		}
	}

	if strings.TrimSpace(os.Getenv("DESKLINK_REQUIRE_TURN_READY")) == "1" &&
		strings.TrimSpace(os.Getenv("DESKLINK_TURN_AUTH_SECRET")) == "" {
		failures = append(failures, "turn-auth-secret")
	}

	return failures
}

func readinessHandler(w http.ResponseWriter, _ *http.Request) {
	failures := readinessFailures()
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	if len(failures) != 0 {
		w.WriteHeader(http.StatusServiceUnavailable)
	}
	_ = json.NewEncoder(w).Encode(readinessResponse{
		OK:     len(failures) == 0,
		Failed: failures,
	})
}

func init() {
	http.HandleFunc("/readyz", readinessHandler)
}
