package main

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
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
	store.setToken("client_001", "registered-token")
	body := bytes.NewBufferString(`{"client_id":"client_001","token":"registered-token"}`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusOK, true, "ok")
}

func TestAuthCheckRejectsInvalidToken(t *testing.T) {
	store = newMemoryStore()
	store.setToken("client_001", "registered-token")
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

func TestAuthCheckRejectsMissingFields(t *testing.T) {
	store = newMemoryStore()
	body := bytes.NewBufferString(`{"client_id":"client_001"}`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusBadRequest, false, "client_id and token are required")
}

func TestAuthCheckRejectsUnregisteredTcpTestToken(t *testing.T) {
	store = newMemoryStore()
	body := bytes.NewBufferString(`{"client_id":"tcp-test-9001","token":"test-token"}`)
	req := httptest.NewRequest(http.MethodPost, "/auth/check", body)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertAuthResponse(t, resp, http.StatusOK, false, "invalid token")
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

func TestClientsReportAndList(t *testing.T) {
	store = newMemoryStore()
	report := `{
		"gateway_id":"gateway-001",
		"clients":[
			{"client_id":"client_001","remote_addr":"127.0.0.1:50001","connected_at":"2026-06-08T12:00:00Z"}
		]
	}`
	req := httptest.NewRequest(http.MethodPost, "/clients/report", bytes.NewBufferString(report))
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	if resp.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d", resp.Code)
	}

	listReq := httptest.NewRequest(http.MethodGet, "/clients", nil)
	listResp := httptest.NewRecorder()

	routes().ServeHTTP(listResp, listReq)

	if listResp.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d", listResp.Code)
	}

	var clients []clientInfo
	if err := json.NewDecoder(listResp.Body).Decode(&clients); err != nil {
		t.Fatalf("decode clients: %v", err)
	}
	if len(clients) != 1 ||
		clients[0].ClientID != "client_001" ||
		clients[0].RemoteAddr != "127.0.0.1:50001" ||
		clients[0].ConnectedAt != "2026-06-08T12:00:00Z" {
		t.Fatalf("unexpected clients: %+v", clients)
	}
}

func TestConfigReload(t *testing.T) {
	store = newMemoryStore()
	router := routes()

	updateReq := httptest.NewRequest(http.MethodPost, "/config", bytes.NewBufferString(`{
		"auth_timeout_ms":1500,
		"max_payload_size":1048576,
		"max_connections_per_client":1,
		"max_requests_per_client_per_second":50,
		"fail_open":true
	}`))
	updateResp := httptest.NewRecorder()
	router.ServeHTTP(updateResp, updateReq)
	assertConfigResponse(t, updateResp, http.StatusOK, runtimeConfig{
		Version:                       2,
		AuthTimeoutMS:                 1500,
		MaxPayloadSize:                1048576,
		MaxConnectionsPerClient:       1,
		MaxRequestsPerClientPerSecond: 50,
		FailOpen:                      true,
	})

	req := httptest.NewRequest(http.MethodPost, "/config/reload", nil)
	resp := httptest.NewRecorder()
	router.ServeHTTP(resp, req)

	if resp.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d", resp.Code)
	}

	var body configReloadResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if !body.Success || body.Message != "memory config reload is a no-op" || body.Version != 2 {
		t.Fatalf("unexpected config reload response: %+v", body)
	}
}

func TestConfigGetReturnsDefault(t *testing.T) {
	store = newMemoryStore()
	req := httptest.NewRequest(http.MethodGet, "/config", nil)
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertConfigResponse(t, resp, http.StatusOK, runtimeConfig{
		Version:                       1,
		AuthTimeoutMS:                 1000,
		MaxPayloadSize:                4194314,
		MaxConnectionsPerClient:       2,
		MaxRequestsPerClientPerSecond: 100,
		FailOpen:                      false,
	})
}

func TestConfigUpdate(t *testing.T) {
	store = newMemoryStore()
	router := routes()

	req := httptest.NewRequest(http.MethodPost, "/config", bytes.NewBufferString(`{
		"auth_timeout_ms":1500,
		"max_payload_size":1048576,
		"max_connections_per_client":1,
		"max_requests_per_client_per_second":50,
		"fail_open":true
	}`))
	resp := httptest.NewRecorder()
	router.ServeHTTP(resp, req)

	assertConfigResponse(t, resp, http.StatusOK, runtimeConfig{
		Version:                       2,
		AuthTimeoutMS:                 1500,
		MaxPayloadSize:                1048576,
		MaxConnectionsPerClient:       1,
		MaxRequestsPerClientPerSecond: 50,
		FailOpen:                      true,
	})

	getReq := httptest.NewRequest(http.MethodGet, "/config", nil)
	getResp := httptest.NewRecorder()
	router.ServeHTTP(getResp, getReq)

	assertConfigResponse(t, getResp, http.StatusOK, runtimeConfig{
		Version:                       2,
		AuthTimeoutMS:                 1500,
		MaxPayloadSize:                1048576,
		MaxConnectionsPerClient:       1,
		MaxRequestsPerClientPerSecond: 50,
		FailOpen:                      true,
	})
}

func TestConfigUpdateRejectsInvalidBody(t *testing.T) {
	store = newMemoryStore()

	for _, body := range []string{
		`{"auth_timeout_ms":`,
		`{
			"auth_timeout_ms":1000,
			"max_payload_size":4194314,
			"max_connections_per_client":2,
			"max_requests_per_client_per_second":100,
			"fail_open":false,
			"unknown":true
		}`,
		`{
			"version":99,
			"auth_timeout_ms":1000,
			"max_payload_size":4194314,
			"max_connections_per_client":2,
			"max_requests_per_client_per_second":100,
			"fail_open":false
		}`,
	} {
		req := httptest.NewRequest(http.MethodPost, "/config", bytes.NewBufferString(body))
		resp := httptest.NewRecorder()

		routes().ServeHTTP(resp, req)

		assertErrorResponse(t, resp, http.StatusBadRequest, "invalid request body")
	}
}

func TestConfigUpdateRejectsInvalidValues(t *testing.T) {
	store = newMemoryStore()

	testCases := []struct {
		name      string
		body      string
		wantError string
	}{
		{
			name: "auth_timeout_ms",
			body: `{
				"auth_timeout_ms":0,
				"max_payload_size":4194314,
				"max_connections_per_client":2,
				"max_requests_per_client_per_second":100,
				"fail_open":false
			}`,
			wantError: "auth_timeout_ms must be positive",
		},
		{
			name: "max_payload_size",
			body: `{
				"auth_timeout_ms":1000,
				"max_payload_size":0,
				"max_connections_per_client":2,
				"max_requests_per_client_per_second":100,
				"fail_open":false
			}`,
			wantError: "max_payload_size must be positive",
		},
		{
			name: "max_connections_per_client",
			body: `{
				"auth_timeout_ms":1000,
				"max_payload_size":4194314,
				"max_connections_per_client":0,
				"max_requests_per_client_per_second":100,
				"fail_open":false
			}`,
			wantError: "max_connections_per_client must be positive",
		},
		{
			name: "max_requests_per_client_per_second",
			body: `{
				"auth_timeout_ms":1000,
				"max_payload_size":4194314,
				"max_connections_per_client":2,
				"max_requests_per_client_per_second":0,
				"fail_open":false
			}`,
			wantError: "max_requests_per_client_per_second must be positive",
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			req := httptest.NewRequest(http.MethodPost, "/config", bytes.NewBufferString(tc.body))
			resp := httptest.NewRecorder()

			routes().ServeHTTP(resp, req)

			assertErrorResponse(t, resp, http.StatusBadRequest, tc.wantError)
		})
	}
}

func TestTokensCRUDAndAuthFlow(t *testing.T) {
	store = newMemoryStore()
	router := routes()

	createReq := httptest.NewRequest(http.MethodPost, "/tokens", bytes.NewBufferString(`{"client_id":"client_001","token":"abc123"}`))
	createResp := httptest.NewRecorder()
	router.ServeHTTP(createResp, createReq)
	assertSuccessResponse(t, createResp, http.StatusOK)

	authReq := httptest.NewRequest(http.MethodPost, "/auth/check", bytes.NewBufferString(`{"client_id":"client_001","token":"abc123"}`))
	authResp := httptest.NewRecorder()
	router.ServeHTTP(authResp, authReq)
	assertAuthResponse(t, authResp, http.StatusOK, true, "ok")

	listReq := httptest.NewRequest(http.MethodGet, "/tokens", nil)
	listResp := httptest.NewRecorder()
	router.ServeHTTP(listResp, listReq)

	if listResp.Code != http.StatusOK {
		t.Fatalf("expected status 200, got %d", listResp.Code)
	}

	rawBody := listResp.Body.String()
	if strings.Contains(rawBody, "abc123") {
		t.Fatalf("token list leaked token value: %s", rawBody)
	}

	var entries []tokenEntry
	if err := json.NewDecoder(strings.NewReader(rawBody)).Decode(&entries); err != nil {
		t.Fatalf("decode token list: %v", err)
	}
	if len(entries) != 1 || entries[0].ClientID != "client_001" {
		t.Fatalf("unexpected token entries: %+v", entries)
	}

	deleteReq := httptest.NewRequest(http.MethodDelete, "/tokens/client_001", nil)
	deleteResp := httptest.NewRecorder()
	router.ServeHTTP(deleteResp, deleteReq)
	assertSuccessResponse(t, deleteResp, http.StatusOK)

	deniedReq := httptest.NewRequest(http.MethodPost, "/auth/check", bytes.NewBufferString(`{"client_id":"client_001","token":"abc123"}`))
	deniedResp := httptest.NewRecorder()
	router.ServeHTTP(deniedResp, deniedReq)
	assertAuthResponse(t, deniedResp, http.StatusOK, false, "invalid token")
}

func TestTokensUpsertRejectsMissingFields(t *testing.T) {
	store = newMemoryStore()
	req := httptest.NewRequest(http.MethodPost, "/tokens", bytes.NewBufferString(`{"client_id":"client_001"}`))
	resp := httptest.NewRecorder()

	routes().ServeHTTP(resp, req)

	assertErrorResponse(t, resp, http.StatusBadRequest, "client_id and token are required")
}

func TestTokenRegistryConcurrentAccess(t *testing.T) {
	store = newMemoryStore()
	var writers sync.WaitGroup
	for i := 0; i < 20; i++ {
		writers.Add(1)
		go func(index int) {
			defer writers.Done()
			clientID := "client_" + string(rune('a'+index))
			store.setToken(clientID, "token")
			_ = store.isAllowed(clientID, "token")
			store.listTokens()
		}(i)
	}
	writers.Wait()
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

func assertErrorResponse(t *testing.T, resp *httptest.ResponseRecorder, wantStatus int, wantError string) {
	t.Helper()

	if resp.Code != wantStatus {
		t.Fatalf("expected status %d, got %d", wantStatus, resp.Code)
	}

	var body errorResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if body.Error != wantError {
		t.Fatalf("expected error %q, got %q", wantError, body.Error)
	}
}

func assertSuccessResponse(t *testing.T, resp *httptest.ResponseRecorder, wantStatus int) {
	t.Helper()

	if resp.Code != wantStatus {
		t.Fatalf("expected status %d, got %d", wantStatus, resp.Code)
	}

	var body successResponse
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if !body.Success {
		t.Fatalf("expected success response, got %+v", body)
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

func assertConfigResponse(t *testing.T, resp *httptest.ResponseRecorder, wantStatus int, want runtimeConfig) {
	t.Helper()

	if resp.Code != wantStatus {
		t.Fatalf("expected status %d, got %d", wantStatus, resp.Code)
	}

	var body runtimeConfig
	if err := json.NewDecoder(resp.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if body != want {
		t.Fatalf("expected config %+v, got %+v", want, body)
	}
}
