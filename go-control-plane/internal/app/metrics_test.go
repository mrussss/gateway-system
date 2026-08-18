package app

import (
	"bytes"
	"errors"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	dto "github.com/prometheus/client_model/go"
	"github.com/prometheus/common/expfmt"
	"github.com/redis/go-redis/v9"
)

func TestMetricsEndpointUsesRegistryAndParses(t *testing.T) {
	storage := newMemoryStore()
	registry := newMetricsRegistry()
	router := routesWithConfigAndMetrics(storage, developmentApplicationConfigFromEnv(), registry)

	request := newTestRequest(http.MethodGet, "/health/live", nil)
	router.ServeHTTP(httptest.NewRecorder(), request)
	assertAuthCode(t, router, "missing-client", "bad-token", "INVALID_CREDENTIALS")
	registry.redisDuration.WithLabelValues("ping").Observe(0.001)
	registry.redisErrors.WithLabelValues("ping").Inc()
	registry.configUpdates.WithLabelValues("success").Inc()

	_, families := scrapeMetrics(t, router)
	wantTypes := map[string]dto.MetricType{
		"control_plane_http_requests_total":              dto.MetricType_COUNTER,
		"control_plane_http_request_duration_seconds":    dto.MetricType_HISTOGRAM,
		"control_plane_http_in_flight_requests":          dto.MetricType_GAUGE,
		"control_plane_redis_operation_duration_seconds": dto.MetricType_HISTOGRAM,
		"control_plane_redis_errors_total":               dto.MetricType_COUNTER,
		"control_plane_auth_total":                       dto.MetricType_COUNTER,
		"control_plane_config_updates_total":             dto.MetricType_COUNTER,
		"go_goroutines":                                  dto.MetricType_GAUGE,
	}
	for name, wantType := range wantTypes {
		family, ok := families[name]
		if !ok {
			t.Fatalf("metric family %q is missing", name)
		}
		if family.GetHelp() == "" || family.GetType() != wantType {
			t.Fatalf("metric %q help=%q type=%s, want type=%s", name, family.GetHelp(), family.GetType(), wantType)
		}
	}
}

func TestHTTPMetricsUseRegisteredRouteNotRawPath(t *testing.T) {
	router := routesWithStore(newMemoryStore())
	sensitiveID := "client-id-must-not-be-an-http-label"
	router.ServeHTTP(httptest.NewRecorder(), newTestRequest(http.MethodGet, "/gateways/"+sensitiveID+"/status", nil))

	body, _ := scrapeMetrics(t, router)
	if strings.Contains(body, sensitiveID) {
		t.Fatalf("raw path value leaked into metrics labels:\n%s", body)
	}
	if !strings.Contains(body, `route="/gateways/{gateway_id}/status"`) {
		t.Fatalf("registered route pattern missing from metrics:\n%s", body)
	}
}

func TestMetricsReportRejectsUnboundedGatewayLabelValue(t *testing.T) {
	router := routesWithStore(newMemoryStore())
	response := httptest.NewRecorder()
	router.ServeHTTP(response, newTestRequest(http.MethodPost, "/metrics/report", strings.NewReader(`{"gateway_id":"invalid/id"}`)))
	if response.Code != http.StatusBadRequest {
		t.Fatalf("invalid gateway_id status=%d body=%s", response.Code, response.Body.String())
	}
}

func TestConfigConflictMetrics(t *testing.T) {
	router := routesWithStore(newMemoryStore())
	body := `{"max_payload_size":1048576,"max_connections_per_client":2,"max_requests_per_client_per_second":100,"slow_client_output_limit":8388608,"log_level":"INFO"}`
	request := newTestRequest(http.MethodPut, "/config", strings.NewReader(body))
	request.Header.Set("If-Match", `"99"`)
	response := httptest.NewRecorder()
	router.ServeHTTP(response, request)
	if response.Code != http.StatusConflict {
		t.Fatalf("config conflict status=%d body=%s", response.Code, response.Body.String())
	}

	metricsBody, _ := scrapeMetrics(t, router)
	if !strings.Contains(metricsBody, `control_plane_config_updates_total{result="conflict"} 1`) ||
		!strings.Contains(metricsBody, "control_plane_config_conflicts_total 1") {
		t.Fatalf("config conflict metrics missing:\n%s", metricsBody)
	}
}

func TestGatewayCollectorExportsSnapshotsAndRemovesExpiredLabels(t *testing.T) {
	storage := &expiringGatewayStore{memoryStore: newMemoryStore()}
	_, err := storage.saveMetrics(metricsReportRequest{
		GatewayID:             "gateway-a",
		ActiveConnections:     3,
		TotalRequests:         12,
		BytesIn:               100,
		BytesOut:              200,
		ErrorCount:            2,
		RequestQueueBacklog:   4,
		ResponseQueueBacklog:  5,
		RequestQueueRejected:  6,
		ResponseQueueRejected: 7,
		SlowClientClosed:      8,
		StaleResponseDropped:  9,
		Timestamp:             time.Now().Add(-defaultGatewayOfflineAfter - time.Second).Unix(),
	})
	if err != nil {
		t.Fatal(err)
	}
	router := routesWithStore(storage)

	body, families := scrapeMetrics(t, router)
	for _, name := range []string{
		"gateway_active_connections",
		"gateway_requests_total",
		"gateway_bytes_in_total",
		"gateway_bytes_out_total",
		"gateway_errors_total",
		"gateway_request_queue_backlog",
		"gateway_response_queue_backlog",
		"gateway_request_queue_rejected_total",
		"gateway_response_queue_rejected_total",
		"gateway_slow_client_closed_total",
		"gateway_stale_response_dropped_total",
		"gateway_last_report_timestamp_seconds",
		"gateway_online",
	} {
		if _, ok := families[name]; !ok {
			t.Errorf("gateway metric family %q is missing", name)
		}
	}
	if !strings.Contains(body, `gateway_online{gateway_id="gateway-a"} 0`) {
		t.Fatalf("offline gateway snapshot not exported correctly:\n%s", body)
	}

	storage.expired.Store(true)
	body, _ = scrapeMetrics(t, router)
	if strings.Contains(body, `gateway_id="gateway-a"`) {
		t.Fatalf("expired gateway label was retained:\n%s", body)
	}
}

func TestPanicRecoveryIsCounted(t *testing.T) {
	registry := newMetricsRegistry()
	mux := http.NewServeMux()
	mux.HandleFunc("GET /panic", func(http.ResponseWriter, *http.Request) {
		panic("test panic")
	})
	handler := middleware(mux, registry)
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, newTestRequest(http.MethodGet, "/panic", nil))
	if response.Code != http.StatusInternalServerError {
		t.Fatalf("panic response status=%d", response.Code)
	}

	metricsResponse := httptest.NewRecorder()
	registry.handler().ServeHTTP(metricsResponse, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	if !strings.Contains(metricsResponse.Body.String(), `control_plane_panics_total{route="/panic"} 1`) {
		t.Fatalf("panic metric missing:\n%s", metricsResponse.Body.String())
	}
}

type expiringGatewayStore struct {
	*memoryStore
	expired atomic.Bool
}

func (s *expiringGatewayStore) listGateways() ([]gatewayStatusResponse, error) {
	if s.expired.Load() {
		return []gatewayStatusResponse{}, nil
	}
	return s.memoryStore.listGateways()
}

func scrapeMetrics(t *testing.T, handler http.Handler) (string, map[string]*dto.MetricFamily) {
	t.Helper()
	response := httptest.NewRecorder()
	handler.ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	if response.Code != http.StatusOK {
		t.Fatalf("metrics status=%d body=%s", response.Code, response.Body.String())
	}
	body := response.Body.String()
	parser := expfmt.TextParser{}
	families, err := parser.TextToMetricFamilies(bytes.NewBufferString(body))
	if err != nil {
		t.Fatalf("Prometheus parser rejected /metrics: %v\n%s", err, body)
	}
	return body, families
}

func TestRedisMetricsHookClassifiesExpectedMisses(t *testing.T) {
	registry := newMetricsRegistry()
	hook := redisMetricsHook{metrics: registry}
	hook.observe("get", time.Now(), redis.Nil)
	hook.observe("ping", time.Now(), errors.New("unavailable"))

	response := httptest.NewRecorder()
	registry.handler().ServeHTTP(response, httptest.NewRequest(http.MethodGet, "/metrics", nil))
	body := response.Body.String()
	if strings.Contains(body, `control_plane_redis_errors_total{operation="get"}`) {
		t.Fatalf("redis.Nil must not be counted as an operation error:\n%s", body)
	}
	if !strings.Contains(body, `control_plane_redis_errors_total{operation="ping"} 1`) {
		t.Fatalf("unexpected Redis failure was not counted:\n%s", body)
	}
}
