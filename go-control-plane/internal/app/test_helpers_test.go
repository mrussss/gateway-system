package app

import (
	"io"
	"net/http"
	"net/http/httptest"
	"testing"
)

func routesWithStore(store Store) http.Handler {
	return routesWithConfig(store, developmentApplicationConfigFromEnv())
}

func setTokenForTest(t testing.TB, store Store, clientID, token string) {
	t.Helper()
	if err := createTokenForTest(store, clientID, token); err != nil {
		t.Fatalf("set token: %v", err)
	}
}

func createTokenForTest(store Store, clientID, token string) error {
	service := newTokenService(developmentApplicationConfigFromEnv().tokenPepper)
	now := nowRFC3339()
	return store.createToken(tokenRecord{
		tokenEntry: tokenEntry{
			ClientID:   clientID,
			Generation: 1,
			CreatedAt:  now,
			UpdatedAt:  now,
		},
		Digest: service.digest(token),
	})
}

func newTestRequest(method, target string, body io.Reader) *http.Request {
	request := httptest.NewRequest(method, target, body)
	if body != nil && (method == http.MethodPost || method == http.MethodPut) {
		request.Header.Set("Content-Type", "application/json")
	}
	return request
}
