# Gateway System

`gateway-system` is a backend infrastructure practice project built around a C++ TCP gateway and a Go control plane.

It is intentionally small in scope: one custom TCP protocol server, one HTTP control plane, Docker Compose for local integration, and smoke tests that exercise the full path.

## Architecture

```text
Client
  |
  | TCP length-prefixed protocol
  | AUTH / PING / ECHO / LOG_PUSH / STATS
  v
C++ Gateway
  | epoll ET + non-blocking socket
  | request queue / worker threads / response queue
  | connection-level AUTH state
  | runtime config puller
  |
  | HTTP JSON
  v
Go Control Plane
  | POST /auth/check
  | POST /metrics/report
  | POST /clients/report
  | POST /tokens
  | GET  /config
  | POST /config
  | GET  /health
  | GET  /gateways
  | GET  /gateways/{gateway_id}/status
  | GET  /gateways/{gateway_id}/clients
  | GET  /gateway/status
  | GET  /clients
  | GET  /tokens
  | DELETE /tokens/{client_id}
  | POST /config/reload
  |
  | Redis client
  v
Redis State Plane
  | tokens
  | runtime config
  | gateway status by gateway_id
  | clients by gateway_id
```

## Quick Start

Run with Docker Compose:

```bash
docker compose up -d --build
```

The gateway listens on `localhost:9000` and the control plane listens on `localhost:8080`.

Run locally without Docker:

```bash
cd go-control-plane
go run .
```

```bash
cd cpp-gateway
cmake -S . -B build
cmake --build build
./build/message_server
```

## Verification

Full smoke test:

```bash
bash scripts/smoke_test.sh
```

This starts Docker Compose, waits for `GET /health`, checks Redis `PING`, `GET /gateway/status`, `GET /gateways`, `GET /gateways/gateway-001/status`, `GET /clients`, and `GET /gateways/gateway-001/clients`, then runs the TCP protocol test against `localhost:9000`.

TCP protocol test only:

```bash
python3 scripts/tcp_protocol_test.py
```

Current protocol checks cover:

- `AUTH`, `PING`, `ECHO`, `LOG_PUSH`, `STATS`
- half-packet and sticky-packet handling
- invalid packet length rejection
- unauthenticated request rejection
- invalid `AUTH` JSON rejection
- missing `AUTH` fields rejection
- invalid `AUTH` field type rejection
- duplicate `AUTH` rejection
- `/clients` reporting of authenticated `client_id`
- `/clients` exclusion of unauthenticated placeholder clients
- repeated `AUTH + PING + close`
- concurrent client `AUTH + ECHO`
- connection close when a second request arrives while `AUTH` is pending
- `/clients` cleanup after an authenticated client disconnects

Component-level checks:

```bash
cd go-control-plane
go test ./...
```

```bash
cd cpp-gateway
cmake -S . -B build
cmake --build build
```

## HTTP API

Health:

```bash
curl http://localhost:8080/health
```

Auth check:

```bash
curl -X POST http://localhost:8080/auth/check \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"test-token"}'
```

Register or update a token:

```bash
curl -X POST http://localhost:8080/tokens \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"test-token"}'
```

Gateway status:

```bash
curl http://localhost:8080/gateway/status
```

Gateways:

```bash
curl http://localhost:8080/gateways
```

Gateway status by id:

```bash
curl http://localhost:8080/gateways/gateway-001/status
```

Clients:

```bash
curl http://localhost:8080/clients
```

Gateway clients by id:

```bash
curl http://localhost:8080/gateways/gateway-001/clients
```

Token registry:

```bash
curl http://localhost:8080/tokens
```

Delete a token:

```bash
curl -X DELETE http://localhost:8080/tokens/client_001
```

Runtime config:

```bash
curl http://localhost:8080/config
```

Update runtime config:

```bash
curl -X POST http://localhost:8080/config \
  -H "Content-Type: application/json" \
  -d '{
    "auth_timeout_ms":1000,
    "max_payload_size":4194314,
    "max_connections_per_client":2,
    "max_requests_per_client_per_second":100,
    "fail_open":false
  }'
```

Reload runtime config:

```bash
curl -X POST http://localhost:8080/config/reload
```

## TCP Protocol

Each packet uses a 4-byte length prefix followed by a fixed header and optional payload:

```text
uint32 body_length
uint8  version
uint8  message_type
uint64 request_id
bytes  payload
```

`body_length` covers `version + message_type + request_id + payload`. Multi-byte integers use network byte order.

Message types:

```text
1  PING       -> 5  PONG
2  ECHO       -> 6  ECHO_RESP
3  LOG_PUSH   -> 8  LOG_ACK
4  STATS      -> 9  STATS_RESP
7  ERROR_RESP
10 AUTH       -> 11 AUTH_RESP
```

## AUTH State Machine

New connections start unauthenticated. Only `AUTH` is accepted before authentication:

```json
{"client_id":"client_001","token":"test-token"}
```

Current behavior:

- Business requests before `AUTH` close the connection.
- `AUTH` is queued to a worker thread, not handled directly in the epoll IO loop.
- The worker validates JSON, required fields, field types, and `client_id + token` correctness through `POST /auth/check`.
- Invalid JSON, missing fields, bad field types, invalid token, or requests sent while `AUTH` is pending close the connection.
- Successful `AUTH` stores the real `client_id` on the connection.
- Repeated `AUTH` after success returns `ERROR_RESP`.
- `/clients` only reports authenticated connections.

## Concurrency Model

- The epoll IO thread owns socket accept, read, write, decode, response draining, and connection close decisions.
- Worker threads only process queued requests and produce `Response` objects.
- `AUTH` is checked in worker threads through `POST /auth/check`; the epoll IO thread does not call the control plane directly.
- Connection records in `connections_` are protected by `connections_mutex_`.
- The metrics reporter thread only reads connection snapshots under the same mutex and reports authenticated clients only.

## Highlights

- C++17 gateway using `epoll` and non-blocking sockets.
- Custom protocol codec with half-packet and sticky-packet handling.
- AUTH-gated request flow with worker-thread dispatch.
- Go control plane using standard `net/http`.
- Redis state plane for tokens, runtime config, gateway status, and clients.
- Multi-gateway status and client APIs backed by Redis.
- MemoryStore kept for local tests and non-Redis runs.
- C++ Gateway currently enforces `max_connections_per_client` and `max_requests_per_client_per_second` only.
- Docker Compose integration and repo-level smoke tests.

## Current Limitations

- AUTH now requires explicit token registration through `POST /tokens`.
- Redis is used for state in Docker Compose, while `MemoryStore` is still available for local tests.
- Docker Compose starts one gateway by default; multiple gateway instances require distinct ports and `GATEWAY_ID` values.
- Multi-gateway APIs expose reported state only; they do not perform service discovery or load balancing.
- `checkAuth()` is synchronous HTTP, although it runs in worker threads instead of the epoll IO thread.
- Connection state is mutex-protected, but the design is still a small in-process model rather than a fully isolated actor-style architecture.
- There is no database, Prometheus, Grafana, or dashboard frontend.
- The main smoke test depends on Docker being available in the local environment.
- The smoke GitHub Actions workflow is manual `workflow_dispatch`, not an every-push integration job.

## Roadmap

- Keep the `AUTH` state machine strict and testable.
- Expand protocol edge-case coverage before changing behavior.
- Improve documentation so project behavior matches real code.
- Add gateway liveness / offline detection.
- Expand smoke test coverage for Redis-backed state.
- Add a manual GitHub Actions smoke workflow without making every push run Docker integration.

## More Docs

- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Development Plan](docs/vibe_coding.md)
