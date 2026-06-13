# Architecture

## Overview

`gateway-system` has two runtime components:

- A C++ TCP gateway that owns client connections and protocol handling.
- A Go control plane that owns HTTP APIs for auth, metrics, and visibility.

## Runtime Flow

```text
Client
  |
  | TCP length-prefixed protocol
  | AUTH / PING / ECHO / LOG_PUSH / STATS
  v
C++ Gateway
  | epoll ET + non-blocking sockets
  | input buffer per connection
  | request queue
  | worker threads
  | response queue
  |
  | HTTP JSON
  v
Go Control Plane
  | POST /auth/check
  | POST /metrics/report
  | POST /clients/report
  | POST /tokens
  | GET  /health
  | GET  /gateway/status
  | GET  /clients
  | GET  /tokens
  | DELETE /tokens/{client_id}
```

## C++ Gateway

Responsibilities:

- accept TCP long connections
- decode the custom binary protocol
- handle half-packets and sticky-packets
- enforce connection-level `AUTH`
- dispatch business requests to worker threads
- send protocol responses back through the IO loop
- report metrics and client snapshots to the control plane

Connection model:

- each connection has buffers for inbound and outbound bytes
- each connection tracks `client_id`, `remote_addr`, `connected_at`
- each connection tracks `authenticated`, `auth_pending`, and `closing`

Threading model:

- the epoll thread accepts sockets, reads bytes, decodes packets, and writes responses
- decoded requests are pushed into a request queue
- worker threads call the dispatcher and business handlers
- responses are pushed into a response queue and drained by the epoll side
- a metrics reporter thread periodically snapshots authenticated clients and gateway counters

Connection state ownership:

- `connections_` is the source of truth for live TCP connections
- `connections_mutex_` guards map lookup, erase, and per-connection state reads or writes
- short critical sections are used for `authenticated`, `auth_pending`, `client_id`, `input_buffer`, `output_buffer`, and `closing`
- blocking or heavier work such as `recv`, `send`, `epoll_ctl`, protocol encoding, and HTTP auth checks stays outside the mutex

## AUTH Path

The current auth path is:

```text
client sends AUTH
  -> epoll thread decodes packet
  -> request queued to worker thread
  -> worker calls POST /auth/check
  -> worker creates AUTH_RESP or close signal
  -> epoll side marks connection authenticated on success
  -> /clients later reports the real client_id
```

Important property:

- `checkAuth()` is synchronous HTTP, but it runs in worker threads rather than the epoll IO loop
- once auth succeeds, the IO side marks the connection authenticated and later client snapshots report that real `client_id`
- if auth is invalid or a second request arrives while auth is pending, the connection is closed

## Go Control Plane

Responsibilities:

- answer `/health`
- validate `client_id + token` through `/auth/check`
- manage token registration through `/tokens`
- store runtime config
- store the latest gateway metrics report
- store the latest authenticated client snapshot
- expose gateway state through `/gateway/status`, `/gateways`, and `/clients`

Current storage model:

- Docker Compose uses Redis for tokens, runtime config, gateway status, and clients
- `MemoryStore` still exists for local tests and non-Redis runs
- Redis stores the latest reported state; derived liveness is computed when status APIs are read

## Deployment Modes

Local mode:

- run the Go control plane directly on `127.0.0.1:8080`
- run the C++ gateway directly on `localhost:9000`

Docker Compose mode:

- expose the gateway on host port `9000`
- expose the control plane on host port `8080`
- let the gateway call the control plane by Compose DNS name

## Known Constraints

- no persistence layer
- no external message broker
- no dashboard frontend
- no async auth client yet
- Docker Compose state depends on Redis availability
- AUTH requires explicit token registration through `POST /tokens`
- `/clients` visibility depends on periodic snapshot reporting rather than an immediate push-on-every-state-change model
- gateway offline state is query-time derived from `last_report_time`
- offline gateway records are not automatically removed from Redis
- smoke testing depends on Docker availability
- GitHub Actions smoke coverage is manual `workflow_dispatch`
