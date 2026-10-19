package app

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
)

func validConfigUpdate() configUpdateRequest {
	return configUpdateRequest{MaxPayloadSize: 1048576, MaxConnectionsPerClient: 4, MaxRequestsPerClientPerSecond: 200, SlowClientOutputLimit: 8388608, LogLevel: "INFO"}
}

func TestConfigHTTPRejectsValuesUnsupportedByGateway(t *testing.T) {
	tests := []struct {
		name   string
		change func(*configUpdateRequest)
	}{
		{"body below fixed header", func(req *configUpdateRequest) { req.MaxPayloadSize = 9 }},
		{"connection limit exceeds int32", func(req *configUpdateRequest) { req.MaxConnectionsPerClient = 1<<32 + 1 }},
		{"rate limit exceeds int32", func(req *configUpdateRequest) { req.MaxRequestsPerClientPerSecond = 1<<32 + 1 }},
		{"output excludes length prefix", func(req *configUpdateRequest) { req.SlowClientOutputLimit = req.MaxPayloadSize + 3 }},
	}
	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			storage := newMemoryStore()
			router := routesWithStore(storage)
			req := validConfigUpdate()
			test.change(&req)
			body, _ := json.Marshal(req)
			request := newTestRequest(http.MethodPut, "/config", bytes.NewReader(body))
			request.Header.Set("If-Match", `"1"`)
			response := httptest.NewRecorder()
			router.ServeHTTP(response, request)
			if response.Code != http.StatusBadRequest {
				t.Errorf("status=%d, want 400; body=%s", response.Code, response.Body.String())
			}
			current, err := storage.getConfig()
			if err != nil || current != defaultRuntimeConfig() {
				t.Fatalf("invalid update changed last good config: config=%+v err=%v", current, err)
			}
		})
	}
}

func TestConfigHTTPAcceptsGatewayBoundaryValues(t *testing.T) {
	for _, bodyLimit := range []int{10, 4194314} {
		storage := newMemoryStore()
		router := routesWithStore(storage)
		req := validConfigUpdate()
		req.MaxPayloadSize = bodyLimit
		req.SlowClientOutputLimit = bodyLimit + 4
		req.MaxConnectionsPerClient = 1<<31 - 1
		req.MaxRequestsPerClientPerSecond = 1<<31 - 1
		body, _ := json.Marshal(req)
		request := newTestRequest(http.MethodPut, "/config", bytes.NewReader(body))
		request.Header.Set("If-Match", `"1"`)
		response := httptest.NewRecorder()
		router.ServeHTTP(response, request)
		if response.Code != http.StatusOK {
			t.Fatalf("body limit %d: status=%d body=%s", bodyLimit, response.Code, response.Body.String())
		}
	}
}

func TestMemoryConfigConcurrentCAS(t *testing.T) {
	storage := newMemoryStore()
	var successes atomic.Int32
	var wait sync.WaitGroup
	for index := 0; index < 20; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if _, err := storage.updateConfig(1, validConfigUpdate()); err == nil {
				successes.Add(1)
			}
		}()
	}
	wait.Wait()
	if successes.Load() != 1 {
		t.Fatalf("expected one update, got %d", successes.Load())
	}
	config, _ := storage.getConfig()
	if config.Version != 2 {
		t.Fatalf("expected version 2, got %d", config.Version)
	}
	if config.RequestQueueCapacityDisplay != 4096 {
		t.Fatalf("display-only request queue capacity changed: %d", config.RequestQueueCapacityDisplay)
	}
}

func TestConfigHTTPPreconditionAndConflict(t *testing.T) {
	router := routesWithStore(newMemoryStore())
	body := `{"max_payload_size":1048576,"max_connections_per_client":4,"max_requests_per_client_per_second":200,"slow_client_output_limit":8388608,"log_level":"INFO"}`
	missing := httptest.NewRecorder()
	router.ServeHTTP(missing, newTestRequest(http.MethodPut, "/config", bytes.NewBufferString(body)))
	if missing.Code != http.StatusPreconditionRequired {
		t.Fatalf("expected 428, got %d", missing.Code)
	}
	request := newTestRequest(http.MethodPut, "/config", bytes.NewBufferString(body))
	request.Header.Set("If-Match", `"9"`)
	conflict := httptest.NewRecorder()
	router.ServeHTTP(conflict, request)
	if conflict.Code != http.StatusConflict {
		t.Fatalf("expected 409, got %d", conflict.Code)
	}
}
