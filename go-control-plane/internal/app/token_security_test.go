package app

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
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
	router := routesWithConfig(newMemoryStore(), developmentApplicationConfigFromEnv())

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

func checkTokenClientIDValidation(t *testing.T, storage Store) {
	t.Helper()
	router := routesWithStore(storage)
	for _, clientID := range []string{"index", strings.Repeat("x", 129)} {
		body, err := json.Marshal(tokenUpsertRequest{ClientID: clientID})
		if err != nil {
			t.Fatal(err)
		}
		requests := []*http.Request{
			newTestRequest(http.MethodPost, "/tokens", bytes.NewReader(body)),
			newTestRequest(http.MethodDelete, "/tokens/"+clientID, nil),
			newTestRequest(http.MethodPost, "/tokens/"+clientID+"/rotate", nil),
		}
		for _, request := range requests {
			request.Header.Set("If-Match", `"1"`)
			response := httptest.NewRecorder()
			router.ServeHTTP(response, request)
			if response.Code != http.StatusBadRequest {
				t.Errorf("%s %s: status=%d, want 400; body=%s", request.Method, request.URL.Path, response.Code, response.Body.String())
			}
		}
	}
	entries, err := storage.listTokens()
	if err != nil || len(entries) != 0 {
		t.Fatalf("invalid client IDs changed token state: entries=%+v err=%v", entries, err)
	}
	// 合法长度边界仍可创建，并证明拒绝保留名后索引可以继续使用。
	body, _ := json.Marshal(tokenUpsertRequest{ClientID: strings.Repeat("v", 128)})
	response := httptest.NewRecorder()
	router.ServeHTTP(response, newTestRequest(http.MethodPost, "/tokens", bytes.NewReader(body)))
	if response.Code != http.StatusCreated {
		t.Fatalf("valid boundary client ID: status=%d body=%s", response.Code, response.Body.String())
	}
}

func TestTokenClientIDValidation(t *testing.T) {
	checkTokenClientIDValidation(t, newMemoryStore())
}

func TestTokenDisableMissingReturnsNotFound(t *testing.T) {
	storage := newMemoryStore()
	router := routesWithStore(storage)
	response := httptest.NewRecorder()
	router.ServeHTTP(response, newTestRequest(http.MethodDelete, "/tokens/missing", nil))
	if response.Code != http.StatusNotFound {
		t.Fatalf("disable missing: status=%d body=%s", response.Code, response.Body.String())
	}
	if err := createTokenForTest(storage, "missing", "valid-token"); err != nil {
		t.Fatalf("disable missing prevented later create: %v", err)
	}
}
