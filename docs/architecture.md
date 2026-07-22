# Architecture

## Components

The system has a C++ TCP data plane, a Go HTTP control plane, and an optional Redis state backend.

```text
Client sockets
     │
     ▼
┌──────────────────── C++ gateway ─────────────────────┐
│ Reactor thread                                       │
│  accept4 / epoll ET / recv / decode / send / close  │
│       │                                   ▲          │
│       ▼                                   │ eventfd  │
│ bounded Request Queue          bounded Response Queue│
│       │                                   ▲          │
│       └────────► Worker pool ─────────────┘          │
└─────────────────────────┬────────────────────────────┘
                          │ HTTP/JSON
                          ▼
                 Go control plane ──► Redis
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
  → bounded request_queue.push
  → Worker dispatch
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

Request overload returns an explicit 503 response. Response overload is treated as a severe delivery failure: the Reactor is notified through an independent deduplicated `fd → conn_id` set and closes the affected connection. No queue failure is silently ignored.

## Runtime config

Config JSON is parsed into a temporary `RuntimeConfig`. Validation must complete before assignment, so readers see either the old complete snapshot or the new complete snapshot. Background updates only apply a strictly higher version. Failed HTTP, JSON, type, or value validation retains the current snapshot.

Currently enforced values are maximum body size, per-client connections, and per-client requests per second. Limits are per gateway process.

## Shutdown

The server lifecycle is `RUNNING → DRAINING → STOPPED`. SIGINT/SIGTERM and programmatic stop set a request and write eventfd, so normal operation can use `epoll_wait(..., -1)` without polling. See [shutdown](shutdown.md) for the drain contract.

## Control plane

The Go service owns token checks, runtime config, latest gateway metrics, and latest authenticated-client snapshots. Docker Compose selects Redis; local runs default to `MemoryStore`. Gateway online/offline status is derived when queried from the age of the latest metrics report.

AUTH HTTP remains synchronous but runs only in Worker threads, never in the Reactor. Metrics/config failures log errors and do not terminate the data plane.
