package app

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"
)

func TestWriteJSONUsesExplicitContentLengthWithoutNewline(t *testing.T) {
	response := httptest.NewRecorder()
	writeJSON(response, http.StatusOK, map[string]bool{"ok": true})

	body := response.Body.String()
	if got := response.Header().Get("Content-Length"); got != strconv.Itoa(len(body)) {
		t.Fatalf("Content-Length=%q, body bytes=%d", got, len(body))
	}
	if len(body) == 0 || body[len(body)-1] == '\n' {
		t.Fatalf("response body must be fixed JSON without encoder newline: %q", body)
	}
}

func TestAuthFailuresAreBoundedAndSuccessClearsCounter(t *testing.T) {
	t.Setenv("AUTH_FAILURE_LIMIT", "3")
	t.Setenv("AUTH_FAILURE_WINDOW_SECONDS", "60")
	storage := newMemoryStore()
	if err := storage.setToken("client-1", "good-token"); err != nil {
		t.Fatal(err)
	}
	router := routesWithStore(storage)

	assertAuthCode(t, router, "client-1", "bad-token", "INVALID_CREDENTIALS")
	assertAuthCode(t, router, "client-1", "bad-token", "INVALID_CREDENTIALS")
	assertAuthCode(t, router, "client-1", "good-token", "OK")
	assertAuthCode(t, router, "client-1", "bad-token", "INVALID_CREDENTIALS")
	assertAuthCode(t, router, "client-1", "bad-token", "INVALID_CREDENTIALS")
	assertAuthCode(t, router, "client-1", "bad-token", "RATE_LIMITED")
	assertAuthCode(t, router, "client-1", "good-token", "RATE_LIMITED")

	metrics := httptest.NewRecorder()
	router.ServeHTTP(metrics, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	if metrics.Code != http.StatusOK ||
		!strings.Contains(metrics.Body.String(), `control_plane_auth_total{result="allowed"} 1`) ||
		!strings.Contains(metrics.Body.String(), "control_plane_auth_rate_limited_total 2") {
		t.Fatalf("unexpected auth metrics:\n%s", metrics.Body.String())
	}
}

func TestAuthFailureCounterExpiresAndConcurrentIncrementsAreNotLost(t *testing.T) {
	storage := newMemoryStore()
	const attempts = 32
	var workers sync.WaitGroup
	workers.Add(attempts)
	for i := 0; i < attempts; i++ {
		go func() {
			defer workers.Done()
			if _, err := storage.recordAuthFailure("client-1", 25*time.Millisecond); err != nil {
				t.Errorf("record failure: %v", err)
			}
		}()
	}
	workers.Wait()

	limited, err := storage.authFailureLimited("client-1", attempts)
	if err != nil || !limited {
		t.Fatalf("expected concurrent count=%d to be limited, limited=%v err=%v", attempts, limited, err)
	}
	time.Sleep(35 * time.Millisecond)
	limited, err = storage.authFailureLimited("client-1", 1)
	if err != nil || limited {
		t.Fatalf("expected expired failure counter, limited=%v err=%v", limited, err)
	}
}

func TestControlPlaneErrorTelemetrySurvivesStatusMapping(t *testing.T) {
	report := metricsReportRequest{
		controlPlaneTelemetry: controlPlaneTelemetry{
			ControlPlaneRequestsAuth:   11,
			ControlPlaneErrorsDeadline: 3,
			ControlPlaneErrorsProtocol: 2,
		},
		GatewayID: "gateway-1",
	}
	status := statusFromMetrics(report)
	body, err := json.Marshal(status)
	if err != nil {
		t.Fatal(err)
	}
	if !strings.Contains(string(body), `"control_plane_errors_deadline":3`) ||
		!strings.Contains(string(body), `"control_plane_requests_auth":11`) {
		t.Fatalf("control-plane telemetry missing from status JSON: %s", body)
	}
	stored := statusToRedis(status)
	if stored["control_plane_errors_protocol"] != int64(2) {
		t.Fatalf("control-plane telemetry missing from Redis mapping: %#v", stored)
	}
}

func assertAuthCode(t *testing.T, handler http.Handler, clientID, token, wantCode string) {
	t.Helper()
	payload, _ := json.Marshal(authCheckRequest{ClientID: clientID, Token: token})
	request := newTestRequest(http.MethodPost, "/auth/check", bytes.NewReader(payload))
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, request)
	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body authCheckResponse
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if body.Code != wantCode {
		t.Fatalf("code=%q, want %q body=%s", body.Code, wantCode, response.Body.String())
	}
}
