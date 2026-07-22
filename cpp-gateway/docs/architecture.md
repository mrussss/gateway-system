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

The Reactor owns connection mutation. Workers perform Dispatcher/Handler work and synchronous control-plane AUTH calls, then publish Response values. Eventfd integrates that cross-thread completion path with epoll without timeout polling.

See the authoritative [system architecture](../../docs/architecture.md) and [design decisions](../../docs/design_decisions.md).
