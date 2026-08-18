package app

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestLiveAndReadyEndpoints(t *testing.T) {
	store = newMemoryStore()
	for _, path := range []string{"/health/live", "/health/ready"} {
		response := httptest.NewRecorder()
		routesWithStore(store).ServeHTTP(response, httptest.NewRequest(http.MethodGet, path, nil))
		if response.Code != http.StatusOK {
			t.Fatalf("%s returned %d", path, response.Code)
		}
	}
}

func TestMiddlewareRejectsNonJSON(t *testing.T) {
	store = newMemoryStore()
	request := httptest.NewRequest(http.MethodPost, "/auth/check", bytes.NewBufferString(`{}`))
	request.Header.Set("Content-Type", "text/plain")
	response := httptest.NewRecorder()
	routesWithStore(store).ServeHTTP(response, request)
	if response.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("expected 415, got %d", response.Code)
	}
}

func TestMiddlewareRejectsMissingContentType(t *testing.T) {
	store = newMemoryStore()
	request := httptest.NewRequest(http.MethodPost, "/auth/check", bytes.NewBufferString(`{}`))
	response := httptest.NewRecorder()
	routesWithStore(store).ServeHTTP(response, request)
	if response.Code != http.StatusUnsupportedMediaType {
		t.Fatalf("expected 415, got %d", response.Code)
	}
	var body apiErrorResponse
	if err := json.NewDecoder(response.Body).Decode(&body); err != nil {
		t.Fatalf("decode response: %v", err)
	}
	if body.RequestID == "" || body.Code != "UNSUPPORTED_MEDIA_TYPE" {
		t.Fatalf("unexpected error response: %+v", body)
	}
}

func TestStrictJSONRejectsTrailingValue(t *testing.T) {
	store = newMemoryStore()
	request := httptest.NewRequest(http.MethodPost, "/auth/check", strings.NewReader(`{} {}`))
	request.Header.Set("Content-Type", "application/json")
	response := httptest.NewRecorder()
	routesWithStore(store).ServeHTTP(response, request)
	if response.Code != http.StatusBadRequest {
		t.Fatalf("expected 400, got %d", response.Code)
	}
}

func TestRoutingErrorsUseTheAPIEnvelope(t *testing.T) {
	store = newMemoryStore()
	router := routesWithStore(store)
	tests := []struct {
		name       string
		method     string
		path       string
		wantStatus int
		wantCode   string
	}{
		{name: "unknown path", method: http.MethodGet, path: "/not-present", wantStatus: http.StatusNotFound, wantCode: "NOT_FOUND"},
		{name: "known path wrong method", method: http.MethodPatch, path: "/config", wantStatus: http.StatusMethodNotAllowed, wantCode: "METHOD_NOT_ALLOWED"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			response := httptest.NewRecorder()
			router.ServeHTTP(response, newTestRequest(test.method, test.path, nil))
			if response.Code != test.wantStatus {
				t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
			}
			var body apiErrorResponse
			if err := json.NewDecoder(response.Body).Decode(&body); err != nil {
				t.Fatalf("decode response: %v; body=%q", err, response.Body.String())
			}
			if body.RequestID == "" || body.Code != test.wantCode || body.Message == "" {
				t.Fatalf("unexpected error response: %+v", body)
			}
		})
	}
}

type unhealthyStore struct{ *memoryStore }

func (s unhealthyStore) Ping(context.Context) error { return errors.New("unavailable") }

func TestReadyFailsWhenStoreIsUnavailable(t *testing.T) {
	store = unhealthyStore{memoryStore: newMemoryStore()}
	response := httptest.NewRecorder()
	routesWithStore(store).ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/health/ready", nil))
	if response.Code != http.StatusServiceUnavailable {
		t.Fatalf("expected 503, got %d", response.Code)
	}
}
