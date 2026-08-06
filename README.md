# Gateway System

`gateway-system` is a compact C++/Go backend system built around a custom TCP data plane and an HTTP control plane. The repository focuses on Linux networking correctness, bounded concurrency, observable overload behavior, deterministic shutdown, and repeatable tests rather than adding more infrastructure products.

## v1.0.0 release scope

Version `v1.0.0` is the completed Phase 0–5 release: contract freeze, Go HTTP
foundation, secure token lifecycle, Redis-backed gateway state, Redis config
CAS, and C++ telemetry/dynamic configuration. This release is sealed at that
scope. Phase 6–9 work is not part of v1.0.0; Prometheus and Kubernetes are not
required components and are not implemented by this version.

## What is implemented

- C++17 TCP gateway using non-blocking sockets, edge-triggered `epoll`, `accept4`, and `eventfd`
- length-prefixed binary protocol with half-packet, sticky-packet, and length validation
- connection-level AUTH and `conn_id` protection against stale responses after fd reuse
- strict synchronous internal HTTP client using non-blocking sockets, `poll`, one absolute deadline, and `Content-Length` framing
- bounded normal Request, AUTH, and Response queues with explicit `OK`, `FULL`, and `STOPPED` results
- independent AUTH Worker pool that isolates slow Go/Redis authentication from ordinary TCP work
- Redis-backed AUTH failure counters with TTL, atomic increments, and stable denial codes
- overload rejection counters and queue capacity/peak/backlog telemetry
- single Reactor ownership of connections; workers never mutate socket state
- offset-based output buffers without per-send copies or front erases
- `RUNNING → DRAINING → STOPPED` shutdown with a configurable deadline
- Go HTTP control plane with Redis or in-memory storage
- runtime config snapshots with validation and monotonic version updates
- CTest unit/integration tests, ASan/UBSan, Go race tests, and push/PR CI
- manual Docker Compose smoke workflow and a two-mode benchmark tool

## Architecture

```text
TCP client
   │ custom protocol
   ▼
C++ Reactor (epoll ET)
   ├─ Connection/input/output state
   ├─ normal Queue ──► normal Workers ─┐
   ├─ AUTH Queue ────► AUTH Workers ───┼─► bounded Response Queue
   │                                   │             │
   └──────────────────── eventfd wakeup ◄────────────┘
                              │
                              ▼ HTTP/JSON
                       Go control plane
                              │
                              ▼
                            Redis
```

The Reactor is the only code that accepts, reads, writes, changes epoll interest, or closes client sockets. A worker returns a value object tagged with both `fd` and `conn_id`; the Reactor discards the response if the fd now belongs to another connection.

See [architecture](docs/architecture.md), [protocol](docs/protocol.md), and [design decisions](docs/design_decisions.md).

## Quick start

Docker Compose:

```bash
docker compose up --build
```

Local development:

```bash
cd go-control-plane
APP_ENV=development go run ./cmd/control-plane
```

```bash
cmake -S cpp-gateway -B cpp-gateway/build -DCMAKE_BUILD_TYPE=Debug
cmake --build cpp-gateway/build --parallel
APP_ENV=development ./cpp-gateway/build/message_server
```

Default endpoints are TCP `localhost:9000`, HTTP `localhost:8080`, and Redis `localhost:6379`.

## Verification

C++ tests:

```bash
cmake -S cpp-gateway -B cpp-gateway/build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build cpp-gateway/build --parallel
ctest --test-dir cpp-gateway/build --output-on-failure
```

Sanitizers:

```bash
cmake -S cpp-gateway -B cpp-gateway/build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON \
  -DGATEWAY_ENABLE_SANITIZERS=ON
cmake --build cpp-gateway/build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir cpp-gateway/build-sanitized --output-on-failure
```

Go and full-system checks:

```bash
(cd go-control-plane && go test ./... && go test -race ./... && go vet ./...)
bash scripts/smoke_test.sh
```

The fast CI workflow runs on every push and pull request. The Docker smoke workflow remains manual because it builds and starts Redis, Go, and C++ containers.

See [testing](docs/testing.md) for the test matrix and [failure cases](docs/failure_cases.md) for verified behavior.

## Overload policy

All three inter-thread queues are bounded and configured independently:

- AUTH Queue full: return `AUTH_RESP` with `AUTH_OVERLOADED` and close after writing; ordinary work never enters this queue.
- Request Queue full: return status `503` for ordinary work.
- Request Queue stopped: treat it as shutdown and reject rather than silently drop.
- Response Queue full/stopped unexpectedly: increment a critical counter, wake the Reactor, and close the matching `fd + conn_id` connection.
- output buffer above the active `slow_client_output_limit`: close that slow connection without affecting other clients.

The STATS response exposes backlog, capacity, process-lifetime peak, rejection, in-flight, outcome, and latency counters.

## Graceful shutdown

SIGINT, SIGTERM, or `TcpServer::stop()` wakes `epoll_wait()` through `eventfd` and enters `DRAINING`. The gateway closes the listener, stops accepting/decoding new work, drains accepted requests and generated responses, flushes output buffers, and exits. A slow client cannot block shutdown beyond `SHUTDOWN_TIMEOUT_MS`. New AUTH calls started while draining are capped by the shutdown deadline; synchronous DNS remains the documented unbounded exception.

See [shutdown](docs/shutdown.md) for exact guarantees and non-guarantees.

## Runtime configuration

| Variable | Default | Meaning |
| --- | ---: | --- |
| `APP_ENV` | `production` | `development`, `test`, `staging`, or `production`; only development permits empty secrets |
| `GATEWAY_PORT` | `9000` | TCP listen port |
| `CONTROL_PLANE_HOST` | `127.0.0.1` | control-plane host |
| `CONTROL_PLANE_PORT` | `8080` | control-plane port |
| `CONTROL_PLANE_TIMEOUT_MS` | `1000` | shared connect/send/receive deadline after DNS resolution (`100`–`30000`) |
| `GATEWAY_ID` | `gateway-001` | reporting identity |
| `GATEWAY_SHARED_TOKEN` | empty | credential sent to gateway-internal control-plane APIs; required unless `APP_ENV=development` |
| `WORKER_COUNT` | auto, max 4 | worker threads; `0` selects auto |
| `AUTH_WORKER_COUNT` | `2` | maximum concurrent control-plane AUTH calls (`1`–`16`) |
| `REQUEST_QUEUE_CAPACITY` | `4096` | accepted work capacity |
| `AUTH_QUEUE_CAPACITY` | `32` | waiting AUTH task capacity |
| `RESPONSE_QUEUE_CAPACITY` | `4096` | completed work capacity |
| `SHUTDOWN_TIMEOUT_MS` | `5000` | graceful shutdown deadline; must be at least `2 × CONTROL_PLANE_TIMEOUT_MS + 100` |
| `GATEWAY_LOG_LEVEL` | `INFO` | set `DEBUG` for per-request metadata |
| `GATEWAY_LOG_PATH` | `logs/access.log` | LOG_PUSH storage path |

Payload, output-buffer, rate, connection, and log-level settings are pulled from the control plane as one immutable validated snapshot. A failed pull or invalid payload leaves the entire active snapshot unchanged; an equal or lower version cannot overwrite a newer one.

The Go control plane uses the same `APP_ENV` contract. Outside development it fails startup unless `CONTROL_PLANE_ADMIN_TOKEN`, `GATEWAY_SHARED_TOKEN`, and `TOKEN_PEPPER` are all non-empty. The legacy `ADMIN_TOKEN` name is not read.

## Performance

The previous implementation drained responses only after `epoll_wait(..., 100)` returned, producing about 100ms latency in a one-connection low-load case. Workers now notify an `EFD_NONBLOCK | EFD_CLOEXEC` eventfd after a successful response push, and normal operation uses an infinite epoll timeout.

The current local Release reference run measured single-connection steady-state P50 `0.28ms` and P95 `0.60ms`. These are local comparison data, not production capacity claims. Method, environment, 10/100-client results, CPU/RSS, and limitations are in [benchmark](docs/benchmark.md).

## Project boundaries

The project intentionally does not add Kubernetes, Kafka, a dashboard, multi-Reactor sharding, TLS, or an asynchronous HTTP client. The synchronous client is deliberately narrow: HTTP/1.0/1.1 JSON, exactly one `Content-Length`, `Connection: close`, no transfer/content encoding, and a fresh TCP connection per call. Synchronous DNS remains outside the socket deadline. These boundaries are deliberate and recorded in [design decisions](docs/design_decisions.md).

## Documentation

- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Testing](docs/testing.md)
- [Shutdown](docs/shutdown.md)
- [Failure cases](docs/failure_cases.md)
- [Benchmark](docs/benchmark.md)
- [Design decisions](docs/design_decisions.md)
- [Development workflow](docs/development_workflow.md)
