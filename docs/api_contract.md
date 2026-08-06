# API Contract

All JSON APIs reject unknown fields and trailing values. Errors use:

```json
{"request_id":"req-...","code":"INVALID_ARGUMENT","message":"invalid request body"}
```

Public endpoints are `GET /health/live` and `GET /health/ready`.
Liveness only proves that the process and HTTP server are alive; readiness also
requires Redis and required configuration.

Gateway-internal endpoints require `X-Gateway-Token`: `POST /auth/check`,
`POST /metrics/report`, `POST /clients/report`, and `GET /config`.

Internal JSON responses are marshaled before headers are committed and always carry an explicit `Content-Length` without an encoder-added newline. AUTH decisions use HTTP 200 with `allowed` plus a stable `code`: `OK`, `INVALID_CREDENTIALS`, `TOKEN_DISABLED`, or `RATE_LIMITED`. Redis/network/protocol failures use HTTP 503 `AUTH_UNAVAILABLE` and are not credential denials.

`GET /metrics` exposes low-cardinality Prometheus counters for allowed, denied, unavailable, rate-limited, and failure-counter-error AUTH outcomes.

Admin endpoints require `Authorization: Bearer <admin-token>`: token create,
list, disable, and rotate; gateway status/client queries; and `GET`/`PUT /config`.
Configuration reads return an ETag containing the quoted version. Updates use
`If-Match`; missing preconditions return 428 and version conflicts return 409.
