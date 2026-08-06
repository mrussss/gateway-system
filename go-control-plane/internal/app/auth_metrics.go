package app

import (
	"fmt"
	"net/http"
	"strconv"
	"strings"
	"sync/atomic"
)

type authMetrics struct {
	allowed              atomic.Uint64
	denied               atomic.Uint64
	unavailable          atomic.Uint64
	rateLimited          atomic.Uint64
	failureCounterErrors atomic.Uint64
}

func (m *authMetrics) record(decision tokenAuthDecision) {
	if decision == tokenAuthAllowed {
		m.allowed.Add(1)
		return
	}
	m.denied.Add(1)
}

func (m *authMetrics) recordUnavailable() { m.unavailable.Add(1) }

func (a *application) handlePrometheusMetrics(w http.ResponseWriter, _ *http.Request) {
	var body strings.Builder
	body.WriteString("# TYPE control_plane_auth_total counter\n")
	fmt.Fprintf(&body, "control_plane_auth_total{result=\"allowed\"} %d\n", a.authMetrics.allowed.Load())
	fmt.Fprintf(&body, "control_plane_auth_total{result=\"denied\"} %d\n", a.authMetrics.denied.Load())
	fmt.Fprintf(&body, "control_plane_auth_total{result=\"unavailable\"} %d\n", a.authMetrics.unavailable.Load())
	body.WriteString("# TYPE control_plane_auth_rate_limited_total counter\n")
	fmt.Fprintf(&body, "control_plane_auth_rate_limited_total %d\n", a.authMetrics.rateLimited.Load())
	body.WriteString("# TYPE control_plane_auth_failure_counter_errors_total counter\n")
	fmt.Fprintf(&body, "control_plane_auth_failure_counter_errors_total %d\n", a.authMetrics.failureCounterErrors.Load())

	payload := body.String()
	w.Header().Set("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
	w.Header().Set("Content-Length", strconv.Itoa(len(payload)))
	w.WriteHeader(http.StatusOK)
	_, _ = w.Write([]byte(payload))
}
