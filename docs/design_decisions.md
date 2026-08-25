# Design decisions

- C++ owns every client socket and epoll mutation; this makes Reactor
  correctness and shutdown ownership explicit.
- Normal work and slow AUTH dependency work use separate bounded queues and
  worker pools.
- eventfd wakes the Reactor after a response enqueue, so idle latency does not
  depend on timeout polling.
- A response is valid only when both fd and monotonic conn_id still match.
- Output buffers use an offset for partial writes and enforce a slow-reader
  limit.
- AUTH is fail-closed on Store or control-plane failure. Redis failure counters
  use atomic increment plus TTL and clear after success.
- Runtime config uses Redis Lua CAS and C++ accepts only validated newer
  versions, retaining last-known-good state otherwise.
- Go uses net/http and MemoryStore as a local/test double; Redis is the shared
  state backend for Compose.
- Internal C++ counters remain useful for tests, overload evidence and
  benchmarks, but there is no remote metrics product surface.
