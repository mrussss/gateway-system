# Failure Cases

This document describes failure cases that already exist in the current implementation. It does not describe recovery logic that the repo does not have.

## Go Control Plane Unavailable

When the C++ gateway cannot reach the Go control plane:

- `AUTH` fails closed because `checkAuth()` treats HTTP request failure as reject
- new connections that send `AUTH` are closed after auth rejection
- periodic metrics reporting fails and only logs an error
- periodic client snapshot reporting fails and only logs an error
- runtime config fetch keeps the current in-process config and logs the failure

Practical effect:

- existing authenticated connections can continue until they disconnect
- new clients cannot authenticate
- `/gateway/status`, `/gateways`, `/clients`, and related views stop receiving fresh reports
- gateway liveness may later appear `offline` because the last successful report gets older than the offline window

There is no fail-open auth path in the current gateway code.

## Redis Unavailable

Docker Compose runs the Go control plane with `STORE_BACKEND=redis`. When Redis is unavailable:

- control-plane handlers that need the store return HTTP `500`
- `/auth/check` returns `{"allowed":false,"reason":"store error"}`
- `/tokens`, `/config`, `/gateway/status`, `/gateways`, `/clients`, and related endpoints return `{"error":"store error"}`

Practical effect:

- new `AUTH` requests are rejected because the gateway sees auth failure
- config updates and config reads fail at the control plane
- gateway metrics and client snapshot writes fail, so visibility data stops updating

The control plane does not switch to `MemoryStore` automatically when Redis fails after startup.

## Gateway Offline

Gateway online or offline state is derived at query time from `last_report_time`.

- a gateway is `online` when the latest metrics report is within the default `15s` window
- otherwise the control plane returns `online: false` and `status: "offline"`
- if `last_report_time` cannot be parsed, the derived view is also offline and `seconds_since_last_report` is `-1`

Practical effect:

- `/gateway/status` and `/gateways/{gateway_id}/status` return `404` before any report has ever been stored
- `/gateways` can still list old gateways from Redis, but their derived status becomes `offline`
- offline records are not automatically removed from Redis

There is no active probing, heartbeat lease cleanup, or service-discovery layer.

## Invalid Token

`AUTH` payload must contain string `client_id` and string `token`.

If the control plane rejects the token:

- `POST /auth/check` returns HTTP `200` with `{"allowed":false,"reason":"invalid token"}`
- the gateway sends an `AUTH` request through a worker thread
- the connection is closed after the auth rejection path completes

Malformed auth is handled differently:

- invalid JSON closes the connection
- missing required fields closes the connection
- invalid field types close the connection

These cases are connection-close failures, not `ERROR_RESP`.

## Config Update Failure

`POST /config` fails in two main ways:

- invalid request JSON returns HTTP `400` with `{"error":"invalid request body"}`
- invalid values return HTTP `400` with one of:
  - `auth_timeout_ms must be positive`
  - `max_payload_size must be positive`
  - `max_connections_per_client must be positive`
  - `max_requests_per_client_per_second must be positive`
- store failures return HTTP `500` with `{"error":"store error"}`

Practical effect:

- the stored runtime config is unchanged when the update fails
- the gateway keeps using its current in-process config until a later successful pull provides a newer version

## Config Pull Failure

The C++ gateway fetches `GET /config` at startup and then in a background polling loop.

If config pull fails:

- startup keeps the built-in default runtime config and logs the failure
- later polling failures keep the current runtime config and log the failure
- invalid or incomplete config payloads from the control plane are rejected by the gateway parser and treated as fetch failure

There is no rollback history, no blocking retry loop before startup, and no push-based config delivery.

## Connection Limit Exceeded

The gateway enforces `max_connections_per_client` after a successful auth result is ready to apply.

When another authenticated connection for the same `client_id` would exceed the limit:

- the gateway returns `AUTH_RESP`
- payload is `{"allowed":false,"reason":"max connections exceeded"}`
- the connection is marked for close after the response is queued

Practical effect:

- existing authenticated connections stay up
- the new connection is rejected during auth completion

This check is per gateway process because it counts authenticated connections in the local `connections_` map.

## Rate Limited

The gateway enforces `max_requests_per_client_per_second` after auth succeeds.

When a client exceeds the per-second window:

- the gateway returns `ERROR_RESP`
- status code is `429`
- payload is `{"status":429,"message":"rate limited"}`
- the connection stays open

The rate-limit window is tracked in memory per gateway process and resets when the current Unix second changes.
