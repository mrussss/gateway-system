# Gateway Telemetry Contract

Version v1.0.0 does not expose a Prometheus `/metrics` endpoint. Gateway
telemetry is reported through `POST /metrics/report`, persisted in Redis, and
returned by the administrative gateway-status APIs.

Each gateway reports current snapshots identified by `gateway_id` and a boot
ID. The snapshot exposes connections, request/byte/error totals, queue
capacity/backlog/peak/rejections, slow-client closes, stale-response drops,
AUTH results, runtime configuration version, process start time, report time,
and online status.

Remote gateway counters are snapshots, not increments. Counter resets are
expected after a gateway restart and are interpreted with the boot ID and
process start time. Request IDs, client tokens, addresses, and other secrets
must not be added to telemetry fields.
