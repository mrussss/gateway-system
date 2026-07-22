# Bounded Graceful Shutdown

## State machine

```text
RUNNING
  │ SIGINT / SIGTERM / stop()
  │ eventfd wake
  ▼
DRAINING
  │ all accepted work and output drained
  │ or SHUTDOWN_TIMEOUT_MS reached
  ▼
STOPPED
```

## Drain sequence

1. The stop request writes eventfd so `epoll_wait(..., -1)` returns immediately.
2. The Reactor enters DRAINING once; repeated stop calls/signals are idempotent.
3. The listener is removed from epoll and closed.
4. Client EPOLLIN interest is removed, so no new business frames are decoded.
5. `request_queue.stop()` rejects new pushes but lets Workers pop every admitted Request.
6. Each Worker pushes its final Response and notifies eventfd. The last Worker stops the Response Queue and notifies again.
7. The Reactor drains responses, enables EPOLLOUT, and sends each pending output buffer.
8. Connections with empty output close. When Workers are gone, the Response Queue is stopped/empty, and all connections are closed, shutdown completes.
9. If the deadline expires first, queued but not-yet-started requests/responses are aborted, remaining connections close, and current Worker calls finish before threads/descriptors are joined or released.

## Guarantees

- Requests successfully admitted before DRAINING are dispatched while the drain remains inside its deadline.
- Responses are attempted until written to the kernel or the deadline expires.
- A slow or non-reading client cannot hold the process beyond the configured deadline plus bounded thread/control-plane cleanup time.
- Repeated `stop()` calls and repeated SIGTERM do not double-close queues or descriptors.

## Non-guarantees

- Bytes still in a client/kernel buffer but not decoded before DRAINING are not accepted work.
- A successful `send()` means bytes entered the kernel socket buffer, not that the remote application consumed them.
- Deadline expiry may truncate pending responses.
- Deadline expiry may discard admitted work that a Worker has not started yet; this is what keeps shutdown bounded under a deep queue.

## Tests

`graceful_shutdown_test` covers idle/repeated stop, a delayed AUTH backlog drained by one Worker, Request Queue overload, generated output held by a slow client, and forced deadline exit.
