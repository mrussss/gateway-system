package app

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func assertAuthResponse(t *testing.T, response *httptest.ResponseRecorder, wantStatus int, wantAllowed bool, wantReason string) {
	t.Helper()
	if response.Code != wantStatus {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body authCheckResponse
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body.Allowed != wantAllowed || body.Reason != wantReason {
		t.Fatalf("auth response=%+v", body)
	}
}

func TestHealth(t *testing.T) {
	response := httptest.NewRecorder()
	routesWithStore(newMemoryStore()).ServeHTTP(response, newTestRequest(http.MethodGet, "/health", nil))
	if response.Code != http.StatusOK || response.Header().Get("Content-Type") != "application/json" {
		t.Fatalf("health status=%d headers=%v body=%s", response.Code, response.Header(), response.Body.String())
	}
}

func TestAuthCheckAllowsValidToken(t *testing.T) {
	store := newMemoryStore()
	setTokenForTest(t, store, "client-1", "registered-token")
	body, _ := json.Marshal(authCheckRequest{ClientID: "client-1", Token: "registered-token"})
	response := httptest.NewRecorder()
	routesWithStore(store).ServeHTTP(response, newTestRequest(http.MethodPost, "/auth/check", bytes.NewReader(body)))
	assertAuthCode(t, routesWithStore(store), "client-1", "registered-token", "OK")
	if response.Code != http.StatusOK {
		t.Fatalf("auth status=%d body=%s", response.Code, response.Body.String())
	}
}

func TestConfigGetReturnsDefault(t *testing.T) {
	response := httptest.NewRecorder()
	routesWithStore(newMemoryStore()).ServeHTTP(response, newTestRequest(http.MethodGet, "/config", nil))
	if response.Code != http.StatusOK || response.Header().Get("ETag") != `"1"` {
		t.Fatalf("config status=%d etag=%q body=%s", response.Code, response.Header().Get("ETag"), response.Body.String())
	}
}
