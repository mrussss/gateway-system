# C++ Data Plane Architecture

```text
epoll Reactor
  ├─ listener and client descriptors
  ├─ Connection map (fd + conn_id)
  ├─ protocol decode and local policy decisions
  ├─ bounded Request Queue → Worker pool
  ├─ eventfd ◄────────────── bounded Response Queue
  └─ offset-based output buffers
```

The Reactor owns connection mutation. Normal Workers dispatch ordinary requests; a separate bounded AUTH Executor performs synchronous control-plane checks. Both groups publish Response values through the shared bounded queue. Eventfd integrates that cross-thread completion path with epoll without timeout polling.

See the authoritative [system architecture](../../docs/architecture.md) and [design decisions](../../docs/design_decisions.md).
