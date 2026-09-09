# Gateway System

Gateway System is a C++17 authenticated long-connection gateway focused on
Linux networking correctness, bounded concurrency, overload protection,
dependency isolation and graceful shutdown. A deliberately small Go control
plane provides token authentication and versioned Redis-backed runtime
configuration.

## Final scope

The C++ data plane owns non-blocking TCP, length-prefixed framing, one Reactor
with edge-triggered `epoll`, bounded normal/AUTH/Response queues, `eventfd`
wakeup, partial writes, backpressure, `fd + conn_id` stale-response
protection, AUTH worker isolation, runtime snapshots and bounded shutdown.

The Go control plane owns only:

- token create/list/disable/rotate with secure random secrets and HMAC-SHA256
  digests;
- `/auth/check`, including Redis-backed failure counting, TTL, atomic increment,
  clear-on-success and fail-closed backend errors;
- Redis Lua CAS runtime configuration with `If-Match`, ETag and monotonic
  versions;
- basic health, HTTP validation and signal/deadline shutdown.

The repository intentionally does not attempt to provide Kubernetes
orchestration, multi-gateway fleet management, online-client aggregation, a
Prometheus monitoring platform, service discovery, distributed rate limiting,
TLS termination, a database business backend, a message queue, multi-Reactor
sharding or production HA Redis.

## Architecture

```text
TCP Client
    │
    ▼
C++ Reactor (epoll ET)
    ├── Normal Queue → Worker ─┐
    ├── AUTH Queue → AUTH Worker ─┤
    └──────── Response Queue ◄────┘
                 │
              eventfd
                 │
                 ▼
              Reactor

AUTH Worker → Go /auth/check → Redis Token + Failure TTL
Go /config  → Redis Lua CAS → C++ validated immutable snapshot
```

The Reactor is the only owner of connection lifetime, epoll interest, socket
I/O and close operations. Workers return values tagged with both the original
fd and monotonic connection id; stale responses are discarded.

See [architecture](docs/architecture.md), [protocol](docs/protocol.md),
[shutdown](docs/shutdown.md), [API contract](docs/api_contract.md) and
[connection lifecycle](docs/connection_lifecycle.md),
[benchmark](docs/benchmark.md) and [design decisions](docs/design_decisions.md).

## Quick start

```bash
cp .env.example .env
docker compose up --build
```

The Compose stack contains Redis, the Go control plane on `:8080`, and the C++
gateway on `:9000`. For local development, use `APP_ENV=development` and
`STORE_BACKEND=memory` with the Go process, then build and run the C++ gateway.

## Verification

Fast gate:

```bash
scripts/release_gate.sh --fast
```

Full gate additionally runs real Redis contracts, Compose smoke, Redis outage
and recovery, and the authenticated C++ benchmark matrix:

```bash
CONTROL_PLANE_ADMIN_TOKEN=... GATEWAY_SHARED_TOKEN=... TOKEN_PEPPER=... \
  scripts/release_gate.sh --full
```

Direct checks:

```bash
(cd go-control-plane && go test ./... && go test -race ./... && go vet ./...)
cmake -S cpp-gateway -B cpp-gateway/build -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build cpp-gateway/build --parallel
ctest --test-dir cpp-gateway/build --output-on-failure
```

Project depth is concentrated on Linux networking correctness, bounded
concurrency, overload behavior, dependency isolation, authentication,
versioned configuration and graceful shutdown. This repository is permanently
frozen at this boundary; future changes are limited to bugs, security issues,
test fixes and interview feedback.
