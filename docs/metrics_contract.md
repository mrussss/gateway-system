# Metrics Contract

The control plane exports `/metrics` with a private registry. HTTP labels are
restricted to `method`, `route`, and `status`; raw URLs, request IDs, client IDs,
addresses, and secrets are forbidden labels.

The gateway collector exports current snapshots with `gateway_id` as its only
identity label. It exposes connections, request/byte/error totals, queue
capacity/backlog/peak/rejections, slow-client closes, stale-response drops,
AUTH results, runtime configuration version, process start time, report time,
and online status.

Remote gateway counters are snapshots, not increments. Counter resets are
therefore expected after a gateway restart and are interpreted with
`gateway_process_start_time_seconds`. A boot ID belongs in logs and Redis, not
in metric labels.
