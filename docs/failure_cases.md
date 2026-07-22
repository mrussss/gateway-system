# Failure Cases

This document states implemented behavior, including its limits.

## Control plane unavailable

- Existing authenticated TCP connections continue to use their in-process state.
- New AUTH checks fail closed.
- Config fetch failure retains the complete previous snapshot.
- Metrics/client report failure logs an error; the Gateway keeps running.
- The control-plane view eventually marks the Gateway offline when its last report ages past the liveness window.

There is no automatic fail-open AUTH path even though `fail_open` remains in the config schema.

## Redis unavailable

With `STORE_BACKEND=redis`, dependent Go handlers return store errors and AUTH returns `allowed=false`. The Go process does not automatically swap to MemoryStore. Redis error behavior is exercised through the Go `errorStore` tests; recovery depends on the Redis client succeeding on later calls.

## Request Queue full

The push returns `FULL`, increments `total_errors` and `request_queue_rejected`, and produces an ERROR response with status 503. If the rejected item is AUTH, the response type is AUTH_RESP and the connection closes after the error is written. Already admitted work is unaffected.

## Response Queue full or stopped unexpectedly

The Worker increments `response_queue_rejected`, records the matching fd and conn_id outside the Response Queue, and writes eventfd. The Reactor closes only that still-matching connection. This avoids silent response loss and prevents a stale close from affecting an fd reused by a new client.

## Slow client

Per-connection pending output is capped at 8 MiB. Exceeding the cap closes that connection. During shutdown, a slow reader may retain pending output only until `SHUTDOWN_TIMEOUT_MS`; the remaining connection is then force-closed.

## Peer and socket errors

Client registrations include `EPOLLRDHUP`; `EPOLLERR`, `EPOLLHUP`, and `EPOLLRDHUP` close the connection. `accept4`, `recv`, `send`, eventfd read/write, and control-plane send/receive handle `EINTR`. All created listener/client/epoll/eventfd descriptors use close-on-exec.

## Invalid protocol

Body lengths below the fixed header or above the compile-time maximum close the connection before allocation. Incomplete frames remain in the input buffer. Complete frames preceding a partial tail are emitted and the tail is retained.

The runtime `max_payload_size` is also checked after decode. A stricter runtime value therefore rejects the connection, although the compile-time guard remains the earliest allocation bound.

## Stale Worker response after fd reuse

Every accepted connection receives a unique conn_id. A response or forced-close record applies only when both fd and conn_id match the current map entry. An old response is discarded rather than sent to the new owner of a reused fd.

## Shutdown deadline

The Gateway stops accepting and reading new work, but drains requests already admitted to the Request Queue. It then drains the Response Queue and output buffers. At the deadline it force-closes remaining connections. The guarantee is bounded best effort, not “every client always receives every response.”
