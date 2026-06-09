package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestHealth(t *testing.T) {
	req := httptest.NewRequest(http.MethodGet, "/health", nil)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	if resp.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d", resp.Code)
	}

	var body healthResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if body.Status != "ok" {
		t.Fatalf("expected health status ok, got %q", body.Status)
	}
}

func TestAuthCheckAllowsValidToken(t *testing.T) {
	body := bytes.NewBufferString(`{"client_id":"client_001","token":"test-token"}`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusOK, true, "ok")
}

func TestAuthCheckRejectsInvalidToken(t *testing.T) {
	body := bytes.NewBufferString(`{"client_id":"client_001","token":"bad-token"}`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusOK, false, "invalid token")
}

func TestAuthCheckRejectsInvalidJSON(t *testing.T) {
	body := bytes.NewBufferString(`{"client_id":`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusBadRequest, false, "invalid request body")
}

func assertAuthResponse(t *testing.T, resp *httptest.ResponseRecorder, wantStatus int, wantAllowed bool, wantReason string) {
	t.Helper()

	if resp.Code != wantStatus {
		t.Fatalf("expected status %d, got %d", wantStatus, resp.Code)
	}

	var body authCheckResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if body.Allowed != wantAllowed || body.Reason != wantReason {
		t.Fatalf("expected allowed=%v reason=%q, got allowed=%v reason=%q",
			wantAllowed, wantReason, body.Allowed, body.Reason)
	}
}
