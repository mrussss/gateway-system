# Interview Notes

## One-sentence summary

I built an authenticated C++17 long-connection TCP data plane with a Go
standard-library control plane, Redis shared state, Prometheus observability,
and a Kubernetes rolling-update contract that makes overload and shutdown
behavior measurable.

## Problem and component split

The project is not a generic API gateway. It demonstrates the difficult parts
of a custom stateful TCP service: non-blocking framing, socket lifetime,
cross-thread responses, bounded overload, secure authentication, atomic config,
dependency failure, and long-connection drain.

- C++ owns `accept4`, edge-triggered epoll, decode/encode, connection state,
  bounded normal/AUTH/Response queues, Worker dispatch, rate/connection limits,
  eventfd wakeups, writes, and deadline-bounded shutdown.
- Go owns strict HTTP APIs, route authentication, token lifecycle, Redis access,
  runtime config CAS, Gateway/client views, health, Prometheus, structured logs,
  panic recovery, and HTTP shutdown.
- Redis stores only token digests, expiring Gateway/client snapshots, the active
  config, and expiring AUTH failure counters.
- Kubernetes runs two Gateway and two Control Plane replicas plus one explicitly
  non-HA Redis StatefulSet/PVC for the demonstration.

## Key design reasoning

One Reactor is the sole socket owner. Workers operate on values and never touch
the connection map or epoll, which makes close and event ordering reviewable.
The tradeoff is that one process does not scale socket IO across Reactors.

An fd is only a reusable table index. Every connection gets a monotonic
`conn_id`; responses and forced-close records must match both. The black-box
test deliberately reuses the exact server fd while an old AUTH is in flight and
proves the old generation is dropped.

All inter-thread queues have fixed capacity and every push result is handled:

- AUTH full returns `AUTH_OVERLOADED` and closes after writing.
- ordinary Request full returns an explicit 503 response.
- Response full increments classified counters, independently notifies the
  Reactor, and closes only the matching `fd + conn_id` connection.

Workers notify eventfd only after a successful response push. The Reactor can
use an infinite `epoll_wait` timeout; one counter wake means “drain until
empty,” so eventfd coalescing does not lose work.

## AUTH and token lifecycle

New connections permit only AUTH. Dedicated bounded AUTH Workers make the
synchronous Go call, so neither the Reactor nor ordinary Workers wait on it.
During Control Plane or Redis failure, established authenticated traffic keeps
using local state while new AUTH fails closed.

`POST /tokens` accepts only `client_id`; Go generates a random secret and
returns it once. Redis stores an HMAC-SHA256 digest plus metadata, never the
secret. Verification uses constant-time comparison. List returns metadata only,
rotate returns one new secret and invalidates the old generation atomically,
and delete disables new AUTH immediately. Admin APIs use Bearer auth; internal
Gateway APIs use a separate shared header secret.

## Runtime config and Redis semantics

The client reads `GET /config`. Admin updates use `PUT /config` with
`If-Match: <version>`. Redis Lua performs expected-version check, increment, and
full-snapshot replacement atomically; stale writers receive 409. C++ parses
into a temporary object and applies only a complete valid strictly newer
snapshot.

Dynamic fields are payload size, per-client connections/rate, slow-client
output limit, and log level. `request_queue_capacity_display` reports the
startup value and is intentionally not a fake runtime resize control.

Gateway status and client snapshots have TTLs. Reports use Pipeline to reduce
round trips, but Pipeline is not a transaction: partial application remains a
documented possibility. Reads clean stale index members, and online state also
checks report age. Views are eventually consistent, not a live socket registry.

## Metrics and cardinality

`/metrics` is public and uses a private Prometheus registry. It exports
Go/process collectors, bounded HTTP/Redis/AUTH/config metrics, and the latest
retained Gateway snapshots. HTTP labels use registered route patterns.
`client_id`, request ID, remote address, raw path, and secrets are forbidden as
labels. Per-Gateway series disappear after the retained report expires; remote
process-lifetime counters may reset when a Gateway restarts.

## Shutdown and Kubernetes

Gateway lifecycle is `RUNNING → DRAINING → STOPPED`. SIGTERM wakes epoll through
eventfd, closes the listener/readiness boundary, stops admitting/decoding new
work, drains accepted work and output, then aborts pending work and force-closes
at the deadline. Slow readers cannot extend the bound. Synchronous DNS before a
non-blocking socket is the documented timing exception.

Kubernetes preStop sends SIGTERM and allows endpoint propagation. TCP readiness
fails when the listener closes; liveness only checks PID 1. With two replicas,
`maxUnavailable: 0`, `maxSurge: 1`, PDB `minAvailable: 1`, a 20-second Gateway
deadline, and 30-second termination grace, new connections move to ready pods.
Existing connections are drained then closed, not migrated, so clients must
reconnect.

The single Redis pod is demonstration state, not a high-availability design.

## Evidence and limits

Local evidence includes CTest/ASan/UBSan, Go Unit/Race/Vet, exact fd reuse,
one-slot Request/Response Queue faults, Control Plane outage, SIGTERM drain,
slow-client deadline, Prometheus parsing/cardinality, and structured benchmark
JSON through 500 clients. Docker smoke/recovery and real Kubernetes rolling
update are mandatory release gates; they are not claimed passed in an
environment without Docker or `kubectl`.

The project does not guarantee TLS, global distributed limits, zero rollout
disconnects, delivery beyond the drain deadline, Redis HA, multi-region
availability, or a production SLO. It intentionally excludes Kafka, SQL,
Gin/GORM, multi-Reactor sharding, service mesh, operators, and dashboards.

## Demo commands

```bash
cp .env.example .env
docker compose up -d --build
bash scripts/smoke_test.sh
```

Create a one-time token:

```bash
curl -X POST http://localhost:8080/tokens \
  -H 'Authorization: Bearer local-admin-change-me' \
  -H 'Content-Type: application/json' \
  -d '{"client_id":"client_001"}'
```

Run protocol, benchmark, and full release evidence:

```bash
python3 scripts/tcp_protocol_test.py
python3 scripts/benchmark_tcp.py --clients 10 --requests-per-client 100
scripts/release_gate.sh --full
```

## Questions I can answer from the code

- Why does a one-Reactor design simplify socket lifetime, and where does it stop scaling?
- Why is `fd + conn_id` necessary even with queued-task cancellation?
- Why is Response Queue overload a connection close rather than a synthetic response?
- Why can eventfd notifications coalesce safely?
- Which Redis operation needs Lua atomicity, and why is Pipeline insufficient?
- What does fail-closed mean for established versus new connections?
- Which metrics are snapshots, which are local counters, and how are labels bounded?
- Why must liveness remain independent from readiness during drain?
- What can and cannot be guaranteed before the shutdown deadline?
- Which evidence would be required before making a production capacity claim?

## Resume bullets

- Built a C++17 TCP Gateway with epoll ET, eventfd Worker wakeups, bounded queues,
  exact fd-generation safety, overload telemetry, and deadline-bounded drain.
- Built a Go `net/http` control plane with generated digest-only tokens, Redis
  TTL/Pipeline/Lua-CAS state, strict middleware, health, Prometheus, and graceful
  shutdown.
- Automated real fault injection, non-root/read-only containers, Redis recovery,
  two-replica Kubernetes rolling drain, and reproducible latency/resource evidence.
