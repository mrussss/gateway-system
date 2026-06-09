package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestHealth(t *testing.T) {
	store = newMemoryStore()
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
	store = newMemoryStore()
	body := bytes.NewBufferString(`{"client_id":"client_001","token":"test-token"}`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusOK, true, "ok")
}

func TestAuthCheckRejectsInvalidToken(t *testing.T) {
	store = newMemoryStore()
	body := bytes.NewBufferString(`{"client_id":"client_001","token":"bad-token"}`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusOK, false, "invalid token")
}

func TestAuthCheckRejectsInvalidJSON(t *testing.T) {
	store = newMemoryStore()
	body := bytes.NewBufferString(`{"client_id":`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusBadRequest, false, "invalid request body")
}

func TestMetricsReportAndGatewayStatus(t *testing.T) {
	store = newMemoryStore()
	report := `{
		"gateway_id":"gateway-001",
		"active_connections":12,
		"total_messages":3456,
		"bytes_in":102400,
		"bytes_out":204800,
		"error_count":3,
		"timestamp":1710000000
	}`
	req := httptest.NewRequest(http.MethodPost, "/metrics/report", bytes.NewBufferString(report))
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertGatewayStatus(t, resp, http.StatusOK, "gateway-001", 12, 3456, "2024-03-09T16:00:00Z")

	statusReq := httptest.NewRequest(http.MethodGet, "/gateway/status", nil)
	statusResp := httptest.NewRecorder()

	routes().ServeHTTP(statusResp, statusReq)

	assertGatewayStatus(t, statusResp, http.StatusOK, "gateway-001", 12, 3456, "2024-03-09T16:00:00Z")
}

func TestGatewayStatusNotReported(t *testing.T) {
	store = newMemoryStore()
	req := httptest.NewRequest(http.MethodGet, "/gateway/status", nil)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	if resp.Code != http.StatusNotFound {
		t.Fatalf("expected status 404, got %d", resp.Code)
	}
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

func assertGatewayStatus(t *testing.T, resp *httptest.ResponseRecorder, wantStatus int, wantGatewayID string, wantConnections int64, wantMessages int64, wantReportTime string) {
	t.Helper()

	if resp.Code != wantStatus {
		t.Fatalf("expected status %d, got %d", wantStatus, resp.Code)
	}

	var body gatewayStatusResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if body.GatewayID != wantGatewayID ||
		body.ActiveConnections != wantConnections ||
		body.TotalMessages != wantMessages ||
		body.LastReportTime != wantReportTime {
		t.Fatalf("unexpected gateway status: %+v", body)
	}
}
