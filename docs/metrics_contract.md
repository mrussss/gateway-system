# v2 Prometheus and Gateway Telemetry Contract

The Go control plane exposes `/metrics` from a private Prometheus registry. It
registers Go runtime/process collectors plus the application metrics below.
Metric names, help text, and type are tested, and the output must parse with the
Prometheus text parser.

## Control-plane metrics

```text
control_plane_http_requests_total{method,route,status}       counter
control_plane_http_request_duration_seconds{method,route}    histogram
control_plane_http_in_flight_requests{method,route}           gauge
control_plane_panics_total{route}                             counter
control_plane_redis_operation_duration_seconds{operation}    histogram
control_plane_redis_errors_total{operation}                   counter
control_plane_auth_total{result}                              counter
control_plane_auth_rate_limited_total                         counter
control_plane_config_updates_total{result}                    counter
control_plane_config_conflicts_total                          counter
```

`result` values are a bounded documented set such as `allowed`, `denied`, and
`unavailable`. `route` is the registered route pattern, never the raw URL.

## Gateway metrics

The latest report for every retained gateway is exported with only `gateway_id`
as the identity label:

```text
gateway_active_connections
gateway_requests_total
gateway_bytes_in_total
gateway_bytes_out_total
gateway_errors_total
gateway_request_queue_backlog
gateway_response_queue_backlog
gateway_request_queue_rejected_total
gateway_response_queue_rejected_total
gateway_slow_client_closed_total
gateway_stale_response_dropped_total
gateway_last_report_timestamp_seconds
gateway_online
```

Gateway cumulative values are remote snapshots, not increments performed by the
control plane. Counter resets are valid after restart and are interpreted using
`gateway_boot_id` and `process_start_time` from the administrative API. A gateway
outside the online window exports `gateway_online 0`; after retention TTL the
collector may delete its label set.

## Cardinality and secrecy rules

Labels must never contain `client_id`, request ID, raw URL, remote address,
client token, shared/admin token, error text, or gateway boot ID. Allowed labels
are fixed low-cardinality route/method/status/operation/result values and the
bounded deployment-level `gateway_id`.

The internal `POST /metrics/report` snapshot also retains queue capacities and
peaks, AUTH executor details, classified control-plane HTTP outcomes, active
configuration version, and shutdown state for diagnostics even when no matching
Prometheus series is required.
