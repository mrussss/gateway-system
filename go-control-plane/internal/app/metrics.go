package app

import (
	"context"
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/prometheus/client_golang/prometheus"
	"github.com/prometheus/client_golang/prometheus/collectors"
	"github.com/prometheus/client_golang/prometheus/promhttp"
	"github.com/redis/go-redis/v9"
)

type authMetrics struct {
	total                *prometheus.CounterVec
	rateLimited          prometheus.Counter
	failureCounterErrors prometheus.Counter
}

func (m *authMetrics) record(decision tokenAuthDecision) {
	result := "denied"
	if decision == tokenAuthAllowed {
		result = "allowed"
	}
	m.total.WithLabelValues(result).Inc()
}

func (m *authMetrics) recordUnavailable() {
	m.total.WithLabelValues("unavailable").Inc()
}

type metricsRegistry struct {
	registry             *prometheus.Registry
	auth                 *authMetrics
	httpRequests         *prometheus.CounterVec
	httpDuration         *prometheus.HistogramVec
	httpInFlight         *prometheus.GaugeVec
	panics               *prometheus.CounterVec
	redisDuration        *prometheus.HistogramVec
	redisErrors          *prometheus.CounterVec
	configUpdates        *prometheus.CounterVec
	configConflicts      prometheus.Counter
	gatewayStoreAttached bool
}

func newMetricsRegistry() *metricsRegistry {
	registry := prometheus.NewRegistry()
	metrics := &metricsRegistry{
		registry: registry,
		httpRequests: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "control_plane_http_requests_total",
			Help: "Total control-plane HTTP requests by method, registered route, and response status.",
		}, []string{"method", "route", "status"}),
		httpDuration: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:    "control_plane_http_request_duration_seconds",
			Help:    "Control-plane HTTP request duration in seconds by method and registered route.",
			Buckets: prometheus.DefBuckets,
		}, []string{"method", "route"}),
		httpInFlight: prometheus.NewGaugeVec(prometheus.GaugeOpts{
			Name: "control_plane_http_in_flight_requests",
			Help: "Current in-flight control-plane HTTP requests by method and registered route.",
		}, []string{"method", "route"}),
		panics: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "control_plane_panics_total",
			Help: "Total recovered control-plane HTTP panics by registered route.",
		}, []string{"route"}),
		redisDuration: prometheus.NewHistogramVec(prometheus.HistogramOpts{
			Name:    "control_plane_redis_operation_duration_seconds",
			Help:    "Redis operation duration in seconds by bounded command name.",
			Buckets: prometheus.DefBuckets,
		}, []string{"operation"}),
		redisErrors: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "control_plane_redis_errors_total",
			Help: "Total unexpected Redis operation errors by bounded command name.",
		}, []string{"operation"}),
		configUpdates: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "control_plane_config_updates_total",
			Help: "Total runtime configuration update attempts by result.",
		}, []string{"result"}),
		configConflicts: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "control_plane_config_conflicts_total",
			Help: "Total runtime configuration compare-and-set conflicts.",
		}),
	}
	metrics.auth = &authMetrics{
		total: prometheus.NewCounterVec(prometheus.CounterOpts{
			Name: "control_plane_auth_total",
			Help: "Total authentication decisions by stable result.",
		}, []string{"result"}),
		rateLimited: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "control_plane_auth_rate_limited_total",
			Help: "Total authentication attempts rejected by the failure limiter.",
		}),
		failureCounterErrors: prometheus.NewCounter(prometheus.CounterOpts{
			Name: "control_plane_auth_failure_counter_errors_total",
			Help: "Total authentication failure-counter storage errors.",
		}),
	}

	registry.MustRegister(
		collectors.NewGoCollector(),
		collectors.NewProcessCollector(collectors.ProcessCollectorOpts{}),
		metrics.httpRequests,
		metrics.httpDuration,
		metrics.httpInFlight,
		metrics.panics,
		metrics.redisDuration,
		metrics.redisErrors,
		metrics.auth.total,
		metrics.auth.rateLimited,
		metrics.auth.failureCounterErrors,
		metrics.configUpdates,
		metrics.configConflicts,
	)
	return metrics
}

func (m *metricsRegistry) handler() http.Handler {
	return promhttp.HandlerFor(m.registry, promhttp.HandlerOpts{EnableOpenMetrics: true})
}

func (m *metricsRegistry) attachGatewayStore(store Store) {
	if m.gatewayStoreAttached {
		return
	}
	m.registry.MustRegister(newGatewayCollector(store))
	m.gatewayStoreAttached = true
}

type gatewayCollector struct {
	store Store

	activeConnections    *prometheus.Desc
	requests             *prometheus.Desc
	bytesIn              *prometheus.Desc
	bytesOut             *prometheus.Desc
	errors               *prometheus.Desc
	requestBacklog       *prometheus.Desc
	responseBacklog      *prometheus.Desc
	requestRejected      *prometheus.Desc
	responseRejected     *prometheus.Desc
	slowClientClosed     *prometheus.Desc
	staleResponseDropped *prometheus.Desc
	lastReport           *prometheus.Desc
	online               *prometheus.Desc
}

func newGatewayCollector(store Store) *gatewayCollector {
	labels := []string{"gateway_id"}
	desc := func(name, help string) *prometheus.Desc {
		return prometheus.NewDesc(name, help, labels, nil)
	}
	return &gatewayCollector{
		store:                store,
		activeConnections:    desc("gateway_active_connections", "Current active TCP connections reported by the gateway."),
		requests:             desc("gateway_requests_total", "Process-lifetime requests reported by the gateway."),
		bytesIn:              desc("gateway_bytes_in_total", "Process-lifetime bytes received by the gateway."),
		bytesOut:             desc("gateway_bytes_out_total", "Process-lifetime bytes sent by the gateway."),
		errors:               desc("gateway_errors_total", "Process-lifetime errors reported by the gateway."),
		requestBacklog:       desc("gateway_request_queue_backlog", "Current gateway Request Queue backlog."),
		responseBacklog:      desc("gateway_response_queue_backlog", "Current gateway Response Queue backlog."),
		requestRejected:      desc("gateway_request_queue_rejected_total", "Process-lifetime Request Queue rejections."),
		responseRejected:     desc("gateway_response_queue_rejected_total", "Process-lifetime Response Queue rejections."),
		slowClientClosed:     desc("gateway_slow_client_closed_total", "Process-lifetime slow-client closures."),
		staleResponseDropped: desc("gateway_stale_response_dropped_total", "Process-lifetime stale responses discarded after fd generation checks."),
		lastReport:           desc("gateway_last_report_timestamp_seconds", "Unix timestamp of the latest retained gateway report."),
		online:               desc("gateway_online", "Whether the latest gateway report is inside the online window (1 or 0)."),
	}
}

func (c *gatewayCollector) Describe(ch chan<- *prometheus.Desc) {
	ch <- c.activeConnections
	ch <- c.requests
	ch <- c.bytesIn
	ch <- c.bytesOut
	ch <- c.errors
	ch <- c.requestBacklog
	ch <- c.responseBacklog
	ch <- c.requestRejected
	ch <- c.responseRejected
	ch <- c.slowClientClosed
	ch <- c.staleResponseDropped
	ch <- c.lastReport
	ch <- c.online
}

func (c *gatewayCollector) Collect(ch chan<- prometheus.Metric) {
	statuses, err := c.store.listGateways()
	if err != nil {
		return
	}
	now := time.Now().UTC()
	for _, status := range statuses {
		gatewayID := status.GatewayID
		if !validGatewayID(gatewayID) {
			continue
		}
		view := gatewayStatusToView(status, now)
		online := 0.0
		if view.Online {
			online = 1
		}
		lastReport := 0.0
		if reportedAt, err := time.Parse(time.RFC3339, status.LastReportTime); err == nil {
			lastReport = float64(reportedAt.Unix())
		}

		ch <- prometheus.MustNewConstMetric(c.activeConnections, prometheus.GaugeValue, nonNegative(status.ActiveConnections), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.requests, prometheus.CounterValue, nonNegative(status.TotalMessages), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.bytesIn, prometheus.CounterValue, nonNegative(status.BytesIn), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.bytesOut, prometheus.CounterValue, nonNegative(status.BytesOut), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.errors, prometheus.CounterValue, nonNegative(status.ErrorCount), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.requestBacklog, prometheus.GaugeValue, nonNegative(status.RequestQueueBacklog), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.responseBacklog, prometheus.GaugeValue, nonNegative(status.ResponseQueueBacklog), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.requestRejected, prometheus.CounterValue, nonNegative(status.RequestQueueRejected), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.responseRejected, prometheus.CounterValue, nonNegative(status.ResponseQueueRejected), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.slowClientClosed, prometheus.CounterValue, nonNegative(status.SlowClientClosed), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.staleResponseDropped, prometheus.CounterValue, nonNegative(status.StaleResponseDropped), gatewayID)
		ch <- prometheus.MustNewConstMetric(c.lastReport, prometheus.GaugeValue, lastReport, gatewayID)
		ch <- prometheus.MustNewConstMetric(c.online, prometheus.GaugeValue, online, gatewayID)
	}
}

func nonNegative(value int64) float64 {
	if value < 0 {
		return 0
	}
	return float64(value)
}

func validGatewayID(value string) bool {
	if len(value) == 0 || len(value) > 64 {
		return false
	}
	for _, character := range value {
		if (character >= 'a' && character <= 'z') ||
			(character >= 'A' && character <= 'Z') ||
			(character >= '0' && character <= '9') ||
			character == '-' || character == '_' || character == '.' {
			continue
		}
		return false
	}
	return true
}

type redisMetricsHook struct {
	metrics *metricsRegistry
}

func (h redisMetricsHook) DialHook(next redis.DialHook) redis.DialHook {
	return next
}

func (h redisMetricsHook) ProcessHook(next redis.ProcessHook) redis.ProcessHook {
	return func(ctx context.Context, cmd redis.Cmder) error {
		started := time.Now()
		err := next(ctx, cmd)
		h.observe(cmd.Name(), started, err)
		return err
	}
}

func (h redisMetricsHook) ProcessPipelineHook(next redis.ProcessPipelineHook) redis.ProcessPipelineHook {
	return func(ctx context.Context, cmds []redis.Cmder) error {
		started := time.Now()
		err := next(ctx, cmds)
		h.observe("pipeline", started, err)
		return err
	}
}

func (h redisMetricsHook) observe(operation string, started time.Time, err error) {
	operation = strings.ToLower(operation)
	if operation == "" {
		operation = "unknown"
	}
	h.metrics.redisDuration.WithLabelValues(operation).Observe(time.Since(started).Seconds())
	if err != nil && !errors.Is(err, redis.Nil) {
		h.metrics.redisErrors.WithLabelValues(operation).Inc()
	}
}

func canonicalRoute(pattern string) string {
	if pattern == "" {
		return "unmatched"
	}
	if _, route, found := strings.Cut(pattern, " "); found {
		return route
	}
	return pattern
}

func statusLabel(status int) string {
	return strconv.Itoa(status)
}
