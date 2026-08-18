# Gateway System

`gateway-system` solves the narrow but difficult problem of running an
authenticated long-connection TCP service whose overload, configuration,
observability, and rolling-shutdown behavior can be explained and reproduced.
It combines a C++ data plane, Go HTTP control plane, Redis shared state,
Prometheus metrics, and a two-replica Kubernetes demonstration. The repository
focuses on Linux networking correctness and evidence, not a claim of universal
production readiness.

## v2 development baseline

Version `v1.0.0` remains the completed Phase 0–5 baseline: Go HTTP engineering,
secure token lifecycle, Redis-backed expiring gateway state, Redis Lua config
CAS, and C++ telemetry/dynamic configuration. The active v2 scope continues from
that implementation instead of rebuilding it. The required destination adds a
complete Prometheus metrics surface, hardened local containers and CI,
Kubernetes deployment, and verified rolling updates with bounded graceful drain.

See [current state](docs/current_state.md) for the evidence-based gap matrix and
the repository-root development plan for the complete checkpoint sequence.

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
- generated one-time tokens stored only as HMAC-SHA256 digests, with rotate and disable
- Redis TTL state, pipelined reports, and Lua compare-and-set runtime configuration
- private-registry Prometheus HTTP/Redis/AUTH/config and Gateway snapshot metrics
- runtime config snapshots with validation and monotonic version updates
- CTest unit/integration tests, ASan/UBSan, Go race tests, and push/PR CI
- non-root/read-only Compose services, Redis outage/recovery automation, and real-Redis CI
- hardened two-replica Kubernetes workloads with PDBs and bounded rolling drain
- authenticated two-mode benchmark and reproducible failure-evidence matrix

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
                       Go control plane ─────► Prometheus scrape
                              │
                              ▼
                   Redis TTL / Pipeline / Lua CAS

Kubernetes: Service → 2 Gateway pods; preStop/SIGTERM → DRAINING → replacement
```

The Reactor is the only code that accepts, reads, writes, changes epoll interest, or closes client sockets. A worker returns a value object tagged with both `fd` and `conn_id`; the Reactor discards the response if the fd now belongs to another connection.

See [architecture](docs/architecture.md), [protocol](docs/protocol.md), and [design decisions](docs/design_decisions.md).

## Why these choices

- C++ owns the latency-sensitive socket path and makes epoll, buffers, queues,
  and shutdown ownership explicit. Go keeps management HTTP, security, Redis,
  and Prometheus code small and reviewable with the standard library.
- One Reactor owns every socket, connection record, and epoll mutation. That
  avoids cross-thread socket lifetime races; this project deliberately trades
  away multi-Reactor horizontal CPU scaling.
- Numeric fds are reused by the kernel, so every accepted connection also gets
  a monotonic `conn_id`. A Worker result applies only when both values still
  match.
- All inter-thread queues are bounded. Overload therefore has a measured,
  explicit rejection or close policy instead of unbounded memory growth.
- Workers write eventfd after a successful Response Queue push. The Reactor can
  block in `epoll_wait` without a fixed polling delay; coalescing is safe because
  one wake means “drain the queue.”
- Runtime config uses a Redis Lua CAS because version check and replacement must
  be atomic. Report Pipeline only reduces round trips and is explicitly not a
  transaction.
- Prometheus labels are limited to bounded route/result/status/operation values
  and validated `gateway_id`; client IDs, request IDs, addresses, secrets, and
  raw paths never become labels.
- Kubernetes removes a terminating pod from readiness when the TCP listener
  closes, while preStop/SIGTERM gives accepted work a bounded drain window. TCP
  sessions are closed, not migrated, so clients must reconnect.

## Quick start

Docker Compose:

```bash
cp .env.example .env
# Replace every placeholder in .env before shared or production-like use.
docker compose up --build
```

The C++ and Go runtime containers use UID/GID 10001, drop capabilities, disable
privilege escalation, and run with read-only root filesystems in Compose. Redis
health gates the control plane, whose live/ready checks in turn gate the gateway.

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

One command runs every release gate when Docker and a Kubernetes cluster are
available:

```bash
CONTROL_PLANE_ADMIN_TOKEN=... GATEWAY_SHARED_TOKEN=... TOKEN_PEPPER=... \
  scripts/release_gate.sh --full
```

`scripts/release_gate.sh --fast` runs the build, CTest, sanitizers, Go
Unit/Race/Vet, static deployment, script, Compose, and documentation checks.
Full mode additionally requires and runs real Redis, Docker smoke/recovery,
Kubernetes deploy/smoke/rolling update, and the benchmark matrix. It never tags
a release automatically.

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
bash scripts/redis_recovery_test.sh
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
| `GATEWAY_LOG_PATH` | empty | LOG_PUSH defaults to stdout; set a path only with a writable mounted directory |

Payload, output-buffer, rate, connection, and log-level settings are pulled from the control plane as one immutable validated snapshot. A failed pull or invalid payload leaves the entire active snapshot unchanged; an equal or lower version cannot overwrite a newer one.

The Go control plane uses the same `APP_ENV` contract. Outside development it fails startup unless `CONTROL_PLANE_ADMIN_TOKEN`, `GATEWAY_SHARED_TOKEN`, and `TOKEN_PEPPER` are all non-empty. The legacy `ADMIN_TOKEN` name is not read.

## Performance

The previous implementation drained responses only after `epoll_wait(..., 100)` returned, producing about 100ms latency in a one-connection low-load case. Workers now notify an `EFD_NONBLOCK | EFD_CLOEXEC` eventfd after a successful response push, and normal operation uses an infinite epoll timeout.

The current local Release/MemoryStore reference measured single-connection
steady-state P50 `0.18ms` and P95 `0.26ms`; a 500-client run completed
10000/10000 requests with P99 `49.44ms`. These are short loopback comparison
data, not production capacity claims. Method, raw samples, payload/slow-reader
cases, CPU/RSS, environment, and limitations are in [benchmark](docs/benchmark.md).

## Prometheus

The public `GET /metrics` endpoint uses a private Prometheus registry and exports
Go runtime/process metrics, low-cardinality HTTP/Redis/AUTH/config metrics, and
the latest retained C++ gateway snapshots. HTTP labels use registered route
patterns; client IDs, request IDs, raw paths, remote addresses, and secrets are
never labels.

```promql
sum(rate(control_plane_http_requests_total[5m])) by (route, status)
histogram_quantile(0.95, sum(rate(control_plane_http_request_duration_seconds_bucket[5m])) by (le, route))
gateway_online == 0
sum(rate(gateway_response_queue_rejected_total[5m])) by (gateway_id)
```

Gateway counters are remote process-lifetime snapshots and may reset when a
gateway restarts. See [metrics contract](docs/metrics_contract.md) for the full
surface and interpretation rules.

## Kubernetes rolling updates

The demonstration deployment runs two Gateway replicas with
`maxUnavailable: 0`, `maxSurge: 1`, and a PDB requiring one available replica.
On pod termination, preStop sends SIGTERM, the Gateway closes TCP 9000 and
becomes unready, accepted work drains within a 20-second deadline, and the pod
has a 30-second termination grace period. Liveness checks process existence, not
the draining listener, so it cannot race readiness by restarting the pod.

```bash
python3 scripts/k8s_manifest_test.py
bash scripts/k8s_deploy.sh
bash scripts/k8s_smoke.sh
bash scripts/k8s_rolling_update_test.sh
```

See the [Kubernetes deployment guide](deploy/kubernetes/README.md) for secrets,
image loading, exact probes, and limitations. The single Redis StatefulSet is a
demonstration with persistent storage, not a high-availability Redis design.

## v2 roadmap and project boundaries

The fixed v2 system consists of the C++ data plane, Go standard-library control
plane, Redis, Prometheus, and Kubernetes rolling updates with graceful drain.
Phases 1–9 are implemented on the incremental v1 foundation and remain under
regression coverage. `results/` contains local C++/Go/sanitizer evidence plus
successful Docker/Redis, 18-scenario container benchmark, and pinned Kind
rolling-update artifacts from GitHub Actions. The release still makes no
production-capacity or high-availability claim.

The project intentionally does not add Kafka, a SQL database, Gin/GORM, an HTTP
reverse proxy, TLS, multi-Reactor sharding, service discovery, service mesh, an
operator, multi-cluster deployment, a Grafana dashboard, automatic fail-open, or
global distributed rate limiting. The synchronous internal client remains
deliberately narrow: HTTP/1.0/1.1 JSON, exactly one `Content-Length`,
`Connection: close`, no transfer/content encoding, and a fresh TCP connection
per call. Synchronous DNS remains outside the socket deadline. These boundaries
are recorded in [design decisions](docs/design_decisions.md).

## Guarantees and non-guarantees

Within the tested contract, frame parsing is bounded, socket ownership is
single-threaded, stale Worker results cannot target a reused fd, queue failures
are observable, new AUTH fails closed during dependency loss, config snapshots
change atomically by version, and shutdown has a deadline. Redis-backed online
views and client lists are eventually consistent snapshots.

The system does not guarantee zero disconnects during a rollout, delivery of
every response after the drain deadline, global rate/connection limits,
multi-region availability, Redis high availability, TLS, synchronous-DNS
deadlines, or production capacity/SLOs. The single Redis StatefulSet and local
benchmark are demonstrations; clients must reconnect and retry according to
their application semantics.

## Documentation

- [Architecture](docs/architecture.md)
- [Protocol](docs/protocol.md)
- [Testing](docs/testing.md)
- [Shutdown](docs/shutdown.md)
- [Failure cases](docs/failure_cases.md)
- [Benchmark](docs/benchmark.md)
- [Design decisions](docs/design_decisions.md)
- [Development workflow](docs/development_workflow.md)
- [Reproducible evidence](results/README.md)
- [Interview notes](docs/interview_notes.md)
