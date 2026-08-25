# API Contract

The Go control plane listens on `:8080`. JSON requests must use
`Content-Type: application/json`; unknown fields, trailing JSON values and
oversized bodies are rejected. Responses include a request id, explicit
`Content-Length` and structured JSON errors.

## Authentication

Admin endpoints require:

```http
Authorization: Bearer <CONTROL_PLANE_ADMIN_TOKEN>
```

The C++ gateway uses:

```http
X-Gateway-Token: <GATEWAY_SHARED_TOKEN>
```

## Health

| Method | Path | Auth | Meaning |
| --- | --- | --- | --- |
| GET | `/health` | none | Process is serving HTTP |
| GET | `/health/ready` | none | Store is reachable and control plane can serve requests |

Readiness returns `503 NOT_READY` when the configured Store cannot be reached.

## AUTH and tokens

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| POST | `/auth/check` | gateway | Check `client_id` and token |
| POST | `/tokens` | admin | Generate and create a token |
| GET | `/tokens` | admin | List token metadata only |
| DELETE | `/tokens/{client_id}` | admin | Disable a token |
| POST | `/tokens/{client_id}/rotate` | admin | Rotate using `If-Match: "<generation>"` |

Token creation returns the one-time secret. Listing never returns a secret or
digest. AUTH returns HTTP 200 with `allowed` and one of `OK`,
`INVALID_CREDENTIALS`, `TOKEN_DISABLED`, `RATE_LIMITED` or
`AUTH_UNAVAILABLE`. Store errors are fail-closed. Failure counts have a
bounded TTL and are cleared after successful authentication.

## Runtime configuration

| Method | Path | Auth | Purpose |
| --- | --- | --- | --- |
| GET | `/config` | admin or gateway | Read active snapshot and ETag |
| PUT | `/config` | admin | Validate and atomically update with If-Match |

`PUT /config` requires the current quoted or unquoted version in
`If-Match`. A stale version returns `409 CONFLICT`; missing precondition
returns `428 PRECONDITION_REQUIRED`. Redis Lua CAS increments the version
monotonically. C++ retains its last-known-good immutable snapshot if fetching or
validating a newer config fails.

The snapshot fields are:

- `version`
- `max_payload_size`
- `max_connections_per_client`
- `max_requests_per_client_per_second`
- `slow_client_output_limit`
- `log_level`
- `request_queue_capacity_display`

The queue capacity display is informational because C++ bounded queues are
allocated at startup.
