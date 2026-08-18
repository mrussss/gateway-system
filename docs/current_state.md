# Current State and v2 Baseline

This document records the Gateway System v2 development baseline at commit
`b94fedd13ee65f98af11e539219289d3563e53c0` on 2026-08-18. Development is
incremental: verified Phase 1–5 behavior is retained and only missing v2 work is
added.

## Verified baseline

- The C++17 data plane owns every socket in one edge-triggered `epoll` Reactor,
  uses `accept4` and `eventfd`, validates `fd + conn_id`, and handles partial,
  combined, and invalid length-prefixed frames.
- Normal, AUTH, and Response queues are bounded and expose backlog, capacity,
  peak, and rejection telemetry. AUTH has an isolated worker pool.
- Shutdown is `RUNNING -> DRAINING -> STOPPED`, closes the listener immediately,
  drains accepted work until a deadline, and force-closes remaining connections.
- The Go standard-library control plane has strict JSON handling, bounded bodies,
  request IDs, structured access logging, panic recovery, route authentication,
  liveness/readiness, graceful HTTP shutdown, and Store cleanup.
- Tokens are generated with `crypto/rand`, stored only as HMAC-SHA256 digests,
  compared in constant time, returned only on create/rotate, and protected by
  generation CAS during rotation.
- Redis gateway status and client snapshots have TTLs; stale index entries are
  cleaned during listing. Reporting uses Pipeline only to reduce round trips.
- Runtime configuration uses `ETag`/`If-Match` and Redis Lua CAS. The C++ gateway
  applies only a complete, valid, strictly newer snapshot.
- C++ telemetry includes queue, AUTH, control-plane client, slow-client,
  stale-response, config-version, and server-state fields.

Baseline evidence on 2026-08-18: Go unit/race/vet passed and all 10 CTest tests
passed. Docker validation was unavailable in the local WSL environment and must
be rerun in a Docker-enabled environment.

## Remaining v2 work

| Phase | State | Remaining work |
| --- | --- | --- |
| 0 | Complete | Final v2 API, Redis, metrics, shutdown, scope, and workflow contracts are frozen in `docs/`. |
| 1–5 | Implemented; retain and regression-test | Close only contract gaps found by tests, including the display-only request queue capacity field. |
| 6 | Complete | Private registry, runtime/process, HTTP, Redis, AUTH/config, gateway snapshot metrics, expiry cleanup, and parser/cardinality tests are implemented. |
| 7 | Implemented; runtime gate pending | Non-root/read-only images, Compose health dependencies, v2 smoke, Redis recovery, and real-Redis CI are present; Docker execution remains required in a Docker-enabled environment. |
| 8 | Implemented; cluster gate pending | Manifests, security/resources, probes, PDBs, drain budget, deploy/smoke scripts, and reconnecting rolling-update test are present; a real cluster run remains required. |
| 9 | Implemented; external release gates pending | Authenticated benchmark matrix, local raw Release evidence, complete fault map, exact queue/fd/SIGTERM tests, final docs, link checker, and staged release gate are present. Docker/Redis and Kubernetes runtime artifacts are still required before `v2.0.0`. |

## Current release evidence

On 2026-08-19, all 10 CTests passed after the added one-slot Request/Response
Queue and exact fd-reuse cases; the shutdown black-box test also passed three
consecutive runs. Go Unit, Race, and Vet passed, including the in-flight HTTP
shutdown/Store Close case. The manifest validator passed all 11 resource
contracts, and the local Release/MemoryStore benchmark completed seven raw
scenarios through 500 clients without a request failure.

The evidence and its limits are under `results/`. The final local environment
had Docker and kubectl clients but no Docker daemon socket or Kubernetes
context, so Docker smoke/Redis recovery and real Kubernetes rolling-update
results remain honest release blockers. The manual
workflows and `scripts/release_gate.sh --full` execute those gates elsewhere;
the repository must not be tagged until successful raw artifacts are committed.

## Fixed v2 scope

The required system is the C++ data plane, Go standard-library control plane,
Redis, Prometheus, and Kubernetes rolling updates with graceful connection
draining. Kafka, SQL databases, Gin/GORM, HTTP reverse proxying, TLS,
multi-Reactor sharding, service discovery, service mesh, operators, multi-cluster
deployment, a Grafana dashboard, automatic fail-open, and global distributed
rate limiting remain outside the project.
