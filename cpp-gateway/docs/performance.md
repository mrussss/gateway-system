# C++ Gateway Performance Notes

The completed performance change is the Response Queue eventfd wakeup.

Historical behavior:

```text
Worker pushes response
  → Reactor remains in epoll_wait(timeout=100ms)
  → low-load response waits for timeout
```

Current behavior:

```text
Worker successfully pushes response
  → write eventfd
  → epoll returns
  → read eventfd until EAGAIN
  → drain all responses
```

Normal operation now uses an infinite epoll timeout. Eventfd also wakes the Reactor for stop requests and for severe Response Queue rejection handling.

The output path uses `output_buffer + write_offset`, avoiding a full-buffer copy and `erase(0, n)` on each partial send. A completed buffer is cleared once; appending after a partial write compacts only the unsent prefix.

Current reference numbers and methodology live in [the root benchmark report](../../docs/benchmark.md). The former ~100ms single-connection delay fell to local Release P50/P95 of 0.28/0.60ms. These are comparison data, not a production capacity claim.

Known performance limits remain fresh-connection synchronous HTTP AUTH in a fixed independent Worker group, synchronous LOG_PUSH file flush, a single Reactor, and a Python benchmark client.
