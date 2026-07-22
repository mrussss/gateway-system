# Connection Lifecycle

```text
accept4(NONBLOCK | CLOEXEC)
  → assign monotonic conn_id
  → register EPOLLIN | EPOLLET | EPOLLRDHUP
  → AUTH_PENDING
  → AUTHENTICATED
  → business requests / output
  → peer close, error, policy close, or shutdown drain
  → epoll DEL + close + erase
```

The fd is only an OS resource number. The connection identity is `(fd, conn_id)`, and every Worker response must match both.

Read events drain `recv` until EAGAIN and decode all complete frames. Write events advance `write_offset` until EAGAIN or completion. ERR/HUP/RDHUP close through the same idempotent `closeConnection` path.

During DRAINING, listener and EPOLLIN interest are removed. Already admitted Request Queue items continue through Workers; output is flushed until empty or the shutdown deadline. See [the root shutdown contract](../../docs/shutdown.md).
