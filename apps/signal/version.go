package main

import (
	"encoding/json"
	"net/http"
)

// buildVersion is injected from the repository VERSION file by release/container
// builds. Development builds keep the explicit fallback instead of pretending to
// be a published release.
var buildVersion = "development"

func versionHandler(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.Header().Set("Cache-Control", "no-store")
	_ = json.NewEncoder(w).Encode(map[string]string{
		"version": buildVersion,
	})
}

func init() {
	http.HandleFunc("/versionz", versionHandler)
}
