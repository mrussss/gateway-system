# Architecture

## Components

The fixed v2 system has a C++ TCP data plane, a Go standard-library HTTP
control plane, Redis shared state, a Prometheus scrape surface, and Kubernetes
rolling deployment semantics. `MemoryStore` exists only for local/unit use.

```text
Client sockets
     │
     ▼
┌──────────────────── C++ gateway ─────────────────────┐
│ Reactor thread                                       │
│  accept4 / epoll ET / recv / decode / send / close  │
│       │                                   ▲          │
│       ▼                                   │ eventfd  │
│ normal Request Queue ─► normal Workers ──┐           │
│ bounded AUTH Queue ───► AUTH Workers ────┼► Response │
│                                           ▲ Queue    │
└─────────────────────────┬────────────────────────────┘
                          │ HTTP/JSON
                          ▼
                 Go control plane ──► Redis
                    │                   ├─ TTL snapshots
                    │                   ├─ report Pipeline
                    │                   └─ config Lua CAS
                    ▼
                 /metrics ◄── Prometheus

Kubernetes Service ─► Gateway pod A
                   └► Gateway pod B
termination: readiness off → listener closed → bounded drain → pod replacement
```

## Ownership and concurrency

The Reactor owns the lifecycle of every `Connection`. Workers receive immutable `Request` values and return `Response` values; they do not access the connection map, call `epoll_ctl`, or write sockets.

`connections_mutex_` protects connection snapshots shared with the metrics thread. Network calls remain non-blocking. Output uses `output_buffer + write_offset`; bytes are cleared only after the whole buffer has been sent.

Each connection receives a monotonically increasing `conn_id`. A Response carries both its original fd and conn_id. The Reactor checks both before applying it, so a delayed Worker response cannot be delivered to a new client that inherited a reused fd.

## Request and response flow

```text
EPOLLIN
  → recv until EAGAIN
  → append input buffer
  → decode every complete frame
  → validate auth/rate/config state
  → AUTH: bounded auth_queue.push; other: request_queue.push
  → dedicated AUTH Worker or normal Worker dispatch
  → bounded response_queue.push
  → eventfd write
  → epoll wakes and drains eventfd/Response Queue
  → validate fd + conn_id
  → append encoded output
  → EPOLLOUT until output is empty
```

The eventfd counter may coalesce many Worker notifications. Coalescing is safe because the notification means “drain the queue,” not “consume exactly one response.” The Reactor reads until `EAGAIN` and then drains the queue until empty.

## Bounded queues

`BlockQueue<T>` has a fixed capacity and returns `PushResult::{OK,FULL,STOPPED}`. `stop()` rejects new pushes but allows consumers to drain elements already queued; a stopped and empty queue makes `pop()` return false.

`abort()` is reserved for shutdown deadline expiry: it stops the queue and discards not-yet-started elements so a deep slow-work backlog cannot make the deadline meaningless.

AUTH overload is handled locally by the Reactor as `AUTH_RESP/AUTH_OVERLOADED` and closes after writing. It never consumes normal Request Queue capacity. Ordinary Request overload returns an explicit 503 response. Response overload is treated as a severe delivery failure: the Reactor is notified through an independent deduplicated `fd → conn_id` set and closes the affected connection. No queue failure is silently ignored.

## Runtime config

Config JSON is parsed into a temporary `RuntimeConfig`. Validation must complete before assignment, so readers see either the old complete snapshot or the new complete snapshot. Background updates only apply a strictly higher version. Failed HTTP, JSON, type, or value validation retains the current snapshot.

The complete snapshot enforces maximum payload size, per-client connections,
per-client requests per second, slow-client output limit, and log level. The
Request Queue capacity field is display-only because a startup-allocated queue
cannot be safely resized by this polling path. Limits are per Gateway process.

## Shutdown

The server lifecycle is `RUNNING → DRAINING → STOPPED`. SIGINT/SIGTERM and programmatic stop set a request and write eventfd, so normal operation can use `epoll_wait(..., -1)` without polling. See [shutdown](shutdown.md) for the drain contract.

## Control plane

The Go service owns generated/digested tokens, runtime config, latest Gateway
metrics, and latest authenticated-client snapshots. Docker and Kubernetes use
Redis; local runs may select `MemoryStore`. Redis status and client keys expire,
stale index members are cleaned during reads, and online/offline is derived
from both retained state and report age.

Gateway reports use Redis Pipeline to reduce network round trips. Pipeline is
not atomic and is never described as a transaction. Config updates use a Lua
compare-and-set because expected-version check, version increment, and complete
replacement must be one atomic Redis operation.

The public Prometheus handler uses a private registry with Go/process,
HTTP/Redis/AUTH/config, and retained Gateway snapshot collectors. Routes use
registered patterns and other label domains are bounded; raw paths, client IDs,
request IDs, remote addresses, and secrets are not labels.

AUTH HTTP remains synchronous but runs only in the dedicated bounded AUTH Executor, never in the Reactor or normal Workers. The internal client keeps sockets non-blocking, uses `poll`, and shares one absolute deadline across all resolved addresses plus connect/send/receive. It requires one valid `Content-Length`, enforces separate 16 KiB header and 1 MiB body limits, and rejects transfer encoding, compression, upgrade, premature EOF, duplicate length, and non-2xx responses. Synchronous `getaddrinfo` is the documented deadline boundary.

Disconnecting an AUTH connection marks its shared cancellation token. A queued task checks that token before contacting Go. Correctness still relies on `fd + conn_id`, because a disconnect can race after the cancellation check.

## Kubernetes drain sequence

The demonstration uses two Gateway replicas, `maxUnavailable: 0`,
`maxSurge: 1`, and a PDB with `minAvailable: 1`. The TCP readiness probe fails
as soon as preStop/SIGTERM closes the listener. The process liveness probe stays
independent of readiness, admitted work drains for at most 20 seconds, and the
30-second termination grace exceeds the 3-second preStop propagation window
plus that drain budget. Existing TCP connections cannot migrate; the automated
client reconnects and bounds observed outage.
