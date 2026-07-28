package app

import (
	"bytes"
	"net/http"
	"net/http/httptest"
	"sync"
	"sync/atomic"
	"testing"
)

func validConfigUpdate() configUpdateRequest {
	return configUpdateRequest{MaxPayloadSize: 1048576, MaxConnectionsPerClient: 4, MaxRequestsPerClientPerSecond: 200, SlowClientOutputLimit: 8388608, LogLevel: "INFO"}
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
