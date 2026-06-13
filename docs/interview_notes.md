# Interview Notes

## One-Sentence Summary

`gateway-system` is a small multi-process backend practice project with a C++ TCP gateway, a Go control plane, and Redis-backed shared state for tokens, runtime config, gateway status, and client views.

## What I Built

- implemented the C++ TCP gateway with a custom length-prefixed protocol
- added connection-level `AUTH` and worker-thread auth checks against the Go control plane
- added runtime config pull from the control plane into the gateway
- added Redis-backed control-plane storage for tokens, config, gateway status, and client snapshots
- added multi-gateway status and per-gateway client visibility APIs
- added repo-level smoke coverage and a lightweight benchmark entry point

## Component Split

`C++ Gateway`

- owns TCP connections, protocol decode/encode, auth state, request dispatch, response writes, rate limiting, and per-process connection limit checks

`Go Control Plane`

- owns HTTP APIs for `/auth/check`, `/tokens`, `/config`, `/metrics/report`, `/clients/report`, `/gateway/status`, `/gateways`, and related read APIs

`Redis`

- stores registered tokens, current runtime config, latest gateway metrics report per gateway, and latest client snapshot per gateway

## AUTH State Machine

- new connections start unauthenticated
- only `AUTH` is allowed before auth succeeds
- `AUTH` is queued to a worker thread, not handled directly in the epoll IO loop
- malformed auth, invalid token, or a second packet while auth is pending closes the connection
- repeated `AUTH` after success returns `ERROR_RESP`
- if auth is valid but the local process already reached `max_connections_per_client`, the gateway returns `AUTH_RESP` with `allowed=false` and closes the new connection

## Runtime Config

- the control plane exposes `GET /config` and `POST /config`
- the gateway pulls config at startup and in a background polling loop
- new config is applied only when the pulled `version` increases
- config pull failure keeps the current in-process config
- `POST /config/reload` currently returns success but is a no-op

## Rate Limit

- after auth succeeds, the gateway enforces `max_requests_per_client_per_second`
- the rate-limit window is tracked in memory per gateway process
- when exceeded, the gateway returns `ERROR_RESP` with status `429`
- the connection stays open after rate-limit rejection

## Multi-Gateway

- the control plane stores gateway status and client snapshots under `gateway_id`
- `GET /gateways` lists all reported gateways
- `GET /gateways/{gateway_id}/status` and `/clients` views are based on the latest reported data
- this is visibility and coordination state only, not load balancing or service discovery

## Liveness

- liveness is derived when status APIs are read
- a gateway is considered online when `last_report_time` is within the default offline window
- stale gateways are shown as offline but are not automatically removed from Redis
- there is no active probing of gateway processes

## Tradeoffs

- `checkAuth()` is synchronous HTTP, but it runs in worker threads so the epoll thread is not blocked on auth calls
- rate limit and connection limit are per gateway process, not globally coordinated across all gateways
- `/clients` is eventually consistent because it reflects periodic snapshots, not live socket enumeration from the control plane
- Redis keeps the design simple for shared state, but the project does not include a durable relational database or analytics stack

## What I Would Improve Next

- split the Go control plane out of one large `main.go` into clearer store, handler, config, and liveness modules
- do a formatting-only cleanup pass on `TcpServer.cpp` before attempting any deeper C++ refactor
- separate config pulling, metrics reporting, and client reporting out of `TcpServer` into narrower gateway-side components
- design a cross-gateway connection limit and rate-limit path if multi-gateway fairness becomes a real requirement
- upgrade the benchmark flow with warmup, broader percentile reporting, and service-side resource sampling
- keep documentation and interview framing aligned with the real implementation instead of overselling production readiness

## Demo Commands

Start the full stack:

```bash
docker compose up -d --build
```

Run smoke verification:

```bash
bash scripts/smoke_test.sh
```

Register a token:

```bash
curl -X POST http://localhost:8080/tokens \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"test-token"}'
```

Run the TCP protocol checks:

```bash
python3 scripts/tcp_protocol_test.py
```

Run the benchmark:

```bash
python3 scripts/benchmark_tcp.py --clients 5 --requests-per-client 10
```

## Interview Questions

- Why keep auth off the epoll thread even though the auth client is synchronous HTTP?
- What consistency model does `/clients` provide, and why is it acceptable here?
- Why is gateway liveness query-time derived instead of persisted as a separate status field?
- What would need to change to make connection limits global across multiple gateways?
- What risks come from periodic config pull instead of a push-based config channel?
- What parts of this design would you replace first in a production system?

## Resume Bullets

- Built a C++17 TCP gateway with `epoll`, worker-thread request processing, and a custom AUTH-gated binary protocol.
- Implemented a Go control plane with Redis-backed APIs for auth, runtime config, gateway metrics, and multi-gateway client visibility.
- Added runtime config pull, per-process rate limiting, connection-limit enforcement, smoke automation, and local benchmark tooling for end-to-end verification.
