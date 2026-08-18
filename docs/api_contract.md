# v2 HTTP API Contract

All JSON APIs require `Content-Type: application/json`, reject bodies over the
configured limit, unknown fields, and trailing JSON values, and return one JSON
value without an encoder-added newline. Errors have the stable shape:

```json
{"request_id":"req-...","code":"INVALID_ARGUMENT","message":"invalid request body"}
```

Every response includes `X-Request-ID`. Store unavailability is a service
failure and never becomes a successful authentication denial.

## Route groups

| Group | Credential | Routes |
| --- | --- | --- |
| Public | None | `GET /health/live`, `GET /health/ready`, `GET /metrics` |
| Internal gateway | `X-Gateway-Token` | `POST /auth/check`, `POST /metrics/report`, `POST /clients/report`, `GET /config` |
| Admin | `Authorization: Bearer <token>` | Token, gateway query, and configuration mutation routes |

Secrets, plaintext client tokens, request bodies containing credentials, and
authorization headers must never be logged or used as metric labels.

## Public endpoints

- `GET /health/live` returns 200 while the process and HTTP server are alive. It
  does not contact Redis.
- `GET /health/ready` returns 200 only when required startup configuration is
  valid and the Store is reachable; otherwise it returns 503.
- `GET /metrics` returns the Prometheus text exposition described in
  [metrics_contract.md](metrics_contract.md).

## Authentication and reporting

- `POST /auth/check` accepts `client_id` and `token`. A completed decision uses
  HTTP 200 and returns `allowed` plus `code`: `OK`, `INVALID_CREDENTIALS`,
  `TOKEN_DISABLED`, or `RATE_LIMITED`. Infrastructure failure returns HTTP 503
  with `AUTH_UNAVAILABLE`.
- `POST /metrics/report` accepts one complete gateway snapshot containing its
  identity, boot/process times, counters, queue gauges/capacities/peaks/rejections,
  AUTH totals, active config version, `server_state`, and report timestamp.
- `POST /clients/report` replaces the reporting gateway's latest client snapshot.
  It is an expiring snapshot, not an event log.

## Token administration

- `POST /tokens` accepts `{"client_id":"..."}`, creates generation 1, and
  returns 201 with the generated plaintext token exactly once. Duplicate IDs
  return 409.
- `GET /tokens` returns metadata only: client ID, generation, timestamps, and
  disabled state. It never returns plaintext or a digest.
- `DELETE /tokens/{client_id}` disables the token immediately for future AUTH.
- `POST /tokens/{client_id}/rotate` requires `If-Match` with the current
  generation. Success returns the new plaintext token exactly once; a concurrent
  generation conflict returns 409 and changes nothing.

## Gateway administration

- `GET /gateways` returns current retained gateway snapshots with derived
  `online`, `status`, and age fields.
- `GET /gateways/{gateway_id}/status` returns one retained snapshot or 404.
- `GET /gateways/{gateway_id}/clients` returns the latest unexpired client
  snapshot or 404.

Legacy convenience routes may remain during v2 development but are not part of
the final v2 public contract.

## Runtime configuration

- `GET /config` returns the complete active snapshot and a quoted version in
  `ETag`.
- `PUT /config` requires `If-Match: <current-version>` (quoted or unquoted) and
  accepts every mutable field. It returns the complete new snapshot and ETag.
  Missing precondition returns 428, stale version returns 409, invalid content
  returns 400, and Store unavailability returns 503.

The final snapshot fields are `version`, `max_payload_size`,
`max_connections_per_client`, `max_requests_per_client_per_second`,
`slow_client_output_limit`, `log_level`, and
`request_queue_capacity_display`. Queue capacity is display-only because the
bounded queue is allocated at process startup. `auth_timeout_ms` and `fail_open`
are not dynamic configuration fields.
