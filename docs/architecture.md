# Architecture

Gateway System has one C++ Linux data plane and a deliberately narrow Go/Redis
control plane. `MemoryStore` is only a unit-test and local-development
double.

## Data path

```text
TCP Client
    │ length-prefixed frames
    ▼
C++ Reactor
    ├── Normal Queue → Worker ─┐
    ├── AUTH Queue → AUTH Worker ─┤
    └──────── Response Queue ◄────┘
                 │
              eventfd
                 │
                 ▼
              Reactor
```

The Reactor owns accept, reads, writes, epoll interest and connection close.
Workers never mutate socket lifetime. Every request and response carries
`fd + conn_id`; the monotonic connection id prevents a delayed response from
being applied after kernel fd reuse.

The normal and AUTH queues are bounded and return explicit `OK`, `FULL` and
`STOPPED` states. AUTH has its own queue and worker pool, so a slow Go/Redis
dependency cannot consume ordinary TCP worker capacity. Worker responses enter
a bounded Response Queue and wake the Reactor through eventfd; the Reactor
drains the queue rather than using fixed-interval polling.

## Backpressure and shutdown

Each connection has an output buffer and write offset. Partial writes retain
the unsent suffix and enable `EPOLLOUT`; the configured output limit protects
against slow readers. The lifecycle is:

```text
RUNNING → DRAINING → STOPPED
```

DRAINING closes the listener, stops new work, drains accepted work and
responses, flushes pending output, and aborts remaining queue items at the
shutdown deadline. The deadline is finite and eventfd wakes the loop for both
responses and stop requests.

## Control plane

```text
AUTH Worker → POST /auth/check → Token digest + Failure TTL in Redis

Admin → PUT /config → validate + If-Match
                  → Redis Lua CAS
                  → version + 1
                  → C++ GET /config
                  → validate → immutable snapshot swap
```

Tokens are generated with a secure random source. Only HMAC-SHA256 digests are
stored. Failed authentication uses atomic Redis `INCR + PEXPIRE`, clears on
success, and fails closed when Redis is unavailable.

Runtime configuration uses an atomic Lua compare-and-set. C++ accepts only a
validated response with a strictly newer version; failed fetches retain the
last-known-good snapshot.

The HTTP service uses Go's `net/http`, strict JSON decoding, structured API
errors, admin/gateway authentication, readiness backed by Store health, and
standard signal/context/deadline shutdown.

## Deliberate boundary

This final project does not include Kubernetes orchestration, a Prometheus
monitoring product, multi-gateway registry or fleet status, online-client
aggregation, service discovery, distributed rate limiting, TLS termination,
business database integration, message queues, multi-Reactor sharding or HA
Redis orchestration.
