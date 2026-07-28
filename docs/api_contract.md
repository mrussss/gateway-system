# API Contract

All JSON APIs reject unknown fields and trailing values. Errors use:

```json
{"request_id":"req-...","code":"INVALID_ARGUMENT","message":"invalid request body"}
```

Public endpoints are `GET /health/live`, `GET /health/ready`, and `GET /metrics`.
Liveness only proves that the process and HTTP server are alive; readiness also
requires Redis and required configuration.

Gateway-internal endpoints require `X-Gateway-Token`: `POST /auth/check`,
`POST /metrics/report`, `POST /clients/report`, and `GET /config`.

Admin endpoints require `Authorization: Bearer <admin-token>`: token create,
list, disable, and rotate; gateway status/client queries; and `GET`/`PUT /config`.
Configuration reads return an ETag containing the quoted version. Updates use
`If-Match`; missing preconditions return 428 and version conflicts return 409.
