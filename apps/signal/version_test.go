package main

import (
	"encoding/json"
	"net/http/httptest"
	"testing"
)

func TestVersionHandlerReturnsInjectedBuildVersion(t *testing.T) {
	original := buildVersion
	buildVersion = "1.0.5-dev"
	t.Cleanup(func() { buildVersion = original })

	recorder := httptest.NewRecorder()
	request := httptest.NewRequest("GET", "/versionz", nil)
	versionHandler(recorder, request)

	if recorder.Code != 200 {
		t.Fatalf("version handler status = %d, want 200", recorder.Code)
	}
	if got := recorder.Header().Get("Cache-Control"); got != "no-store" {
		t.Fatalf("Cache-Control = %q, want no-store", got)
	}
	var body map[string]string
	if err := json.Unmarshal(recorder.Body.Bytes(), &body); err != nil {
		t.Fatalf("decode version response: %v", err)
	}
	if got := body["version"]; got != "1.0.5-dev" {
		t.Fatalf("version = %q, want 1.0.5-dev", got)
	}
}
