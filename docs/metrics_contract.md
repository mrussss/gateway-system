# Gateway Telemetry Contract

Gateway telemetry is reported through `POST /metrics/report`, persisted in Redis, and returned by the administrative gateway-status APIs. The Go control plane also exposes a small Prometheus `/metrics` surface for AUTH security counters.

Each gateway reports current snapshots identified by `gateway_id` and a boot
ID. The snapshot exposes connections, request/byte/error totals, queue
capacity/backlog/peak/rejections for the normal, AUTH, and response queues; AUTH in-flight/cancelled/outcome/latency totals; response rejection source; slow-client closes; stale-response drops; runtime configuration version; process start time; report time; and online status.

Prometheus counters are `control_plane_auth_total{result="allowed|denied|unavailable"}`, `control_plane_auth_rate_limited_total`, and `control_plane_auth_failure_counter_errors_total`. Labels remain low-cardinality and never contain client IDs, request IDs, paths, addresses, or tokens.

Remote gateway counters are snapshots, not increments. Counter resets are
expected after a gateway restart and are interpreted with the boot ID and
process start time. Request IDs, client tokens, addresses, and other secrets
must not be added to telemetry fields.
