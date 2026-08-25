# Failure cases

- Invalid framing is rejected and the connection is closed without touching
  worker queues.
- A full Request or AUTH queue returns an explicit overload response while
  retaining queue isolation.
- A full Response Queue closes only the affected connection through the
  Reactor's rejected-response path.
- A slow reader is bounded by its output limit and the shutdown deadline.
- A stale worker response is dropped unless both fd and conn_id still identify
  the same live connection.
- Control-plane HTTP timeout, malformed JSON, non-2xx status or Redis failure
  fails AUTH closed and does not terminate the C++ process.
- Redis outage makes readiness fail, preserves established C++ data-plane
  traffic, rejects new AUTH with AUTH_UNAVAILABLE, and recovers on later
  successful calls.
- Invalid or stale runtime configuration is rejected; the active C++ snapshot
  remains last-known-good.
- SIGINT/SIGTERM transitions the C++ server through bounded DRAINING and the
  Go server uses context cancellation plus an HTTP shutdown deadline.
