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

func TestTokenDigestAndRotateCAS(t *testing.T) {
	t.Setenv("TOKEN_PEPPER", "test-pepper")
	storage := newMemoryStore()
	service := newTokenService("test-pepper")
	now := nowRFC3339()
	if err := storage.createToken(tokenRecord{tokenEntry: tokenEntry{ClientID: "client-1", Generation: 1, CreatedAt: now, UpdatedAt: now}, Digest: service.digest("original")}); err != nil {
		t.Fatal(err)
	}
	if storage.tokens["client-1"].Digest == "original" {
		t.Fatal("plaintext token stored")
	}

	var successes atomic.Int32
	var wait sync.WaitGroup
	for index := 0; index < 20; index++ {
		wait.Add(1)
		go func() {
			defer wait.Done()
			if _, err := storage.rotateToken("client-1", 1, service.digest("rotated"), nowRFC3339()); err == nil {
				successes.Add(1)
			}
		}()
	}
	wait.Wait()
	if successes.Load() != 1 {
		t.Fatalf("expected one rotate, got %d", successes.Load())
	}
}

func TestTokenReturnedOnceAndDisableBlocksAuth(t *testing.T) {
	t.Setenv("TOKEN_PEPPER", "test-pepper")
	storage := newMemoryStore()
	router := routesWithStore(storage)
	create := newTestRequest(http.MethodPost, "/tokens", bytes.NewBufferString(`{"client_id":"client-1"}`))
	response := httptest.NewRecorder()
	router.ServeHTTP(response, create)
	if response.Code != http.StatusCreated {
		t.Fatalf("create status %d", response.Code)
	}
	var secret tokenSecretResponse
	if err := json.NewDecoder(response.Body).Decode(&secret); err != nil {
		t.Fatal(err)
	}

	disable := httptest.NewRecorder()
	router.ServeHTTP(disable, newTestRequest(http.MethodDelete, "/tokens/client-1", nil))
	request := newTestRequest(http.MethodPost, "/auth/check", bytes.NewBufferString(`{"client_id":"client-1","token":"`+secret.Token+`"}`))
	auth := httptest.NewRecorder()
	router.ServeHTTP(auth, request)
	assertAuthResponse(t, auth, http.StatusOK, false, "token disabled")
}

func TestAdminAndGatewayAuthentication(t *testing.T) {
	t.Setenv("CONTROL_PLANE_ADMIN_TOKEN", "admin-secret")
	t.Setenv("GATEWAY_SHARED_TOKEN", "gateway-secret")
	router := routesWithStore(newMemoryStore())

	admin := httptest.NewRecorder()
	router.ServeHTTP(admin, newTestRequest(http.MethodGet, "/tokens", nil))
	if admin.Code != http.StatusUnauthorized {
		t.Fatalf("expected admin 401, got %d", admin.Code)
	}

	gateway := httptest.NewRecorder()
	request := newTestRequest(http.MethodPost, "/auth/check", bytes.NewBufferString(`{"client_id":"c","token":"t"}`))
	router.ServeHTTP(gateway, request)
	if gateway.Code != http.StatusUnauthorized {
		t.Fatalf("expected gateway 401, got %d", gateway.Code)
	}
}
