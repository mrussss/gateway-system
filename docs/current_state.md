# Current state

The repository is permanently frozen at the Final Scope boundary.

## Owned capabilities

- C++17 Linux data plane: TCP, non-blocking sockets, epoll ET, framing, one
  Reactor, bounded normal/AUTH/Response queues, eventfd, partial writes,
  backpressure, fd plus conn_id validation and graceful shutdown.
- Go/Redis control plane: token lifecycle, AUTH decisions, failure TTL, runtime
  config validation, Redis Lua CAS, monotonic versions, health and shutdown.
- Evidence: CTest, ASan/UBSan, Go unit/race/vet, real Redis contracts, Compose
  smoke, failure/recovery tests and C++ benchmark scenarios.

## Explicit non-goals

Kubernetes deployment, Prometheus product monitoring, multi-gateway registry,
online-client aggregation, service discovery, distributed rate limiting, TLS
termination, business database integration, message queues, multi-Reactor
sharding and production HA Redis are outside ownership.

## Maintenance rule

After the final tag, only bugs, security fixes, test fixes and interview
feedback may change the project. The complete pre-reduction v2 implementation
remains available from archive/v2-full and v2-full-archive.
