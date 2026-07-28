package app

import (
	"bytes"
	"context"
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
