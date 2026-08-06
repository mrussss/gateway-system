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

With `STORE_BACKEND=redis`, dependent Go handlers return store errors and AUTH returns HTTP 503 `AUTH_UNAVAILABLE`. Infrastructure failures do not increment the credential-failure counter. The Go process does not automatically swap to MemoryStore; recovery happens when later Redis calls succeed.

## AUTH Queue full

The push returns `FULL`, increments `auth_queue_rejected`, and the Reactor locally produces `AUTH_RESP` with `AUTH_OVERLOADED` and closes after writing. It does not touch the normal Request or shared Response Queue.

## Request Queue full

The push returns `FULL`, increments `total_errors` and `request_queue_rejected`, and produces an ERROR response with status 503. AUTH never enters this queue. Already admitted ordinary work is unaffected.

## Response Queue full or stopped unexpectedly

The Worker increments `response_queue_rejected`, records the matching fd and conn_id outside the Response Queue, and writes eventfd. The Reactor closes only that still-matching connection. This avoids silent response loss and prevents a stale close from affecting an fd reused by a new client.

## Slow client

Per-connection pending output is capped at 8 MiB. Exceeding the cap closes that connection. During shutdown, a slow reader may retain pending output only until `SHUTDOWN_TIMEOUT_MS`; the remaining connection is then force-closed.

## Control-plane HTTP errors

Resolve, connect, deadline, send, receive, HTTP framing, non-2xx status, JSON, and size failures remain distinct internally and in logs. Every one maps AUTH to `Unavailable`; only a successful 2xx JSON response with `allowed=false` maps to `Denied`.

## Peer and socket errors

Client registrations include `EPOLLRDHUP`; `EPOLLERR`, `EPOLLHUP`, and `EPOLLRDHUP` close the connection. `accept4`, `recv`, `send`, eventfd read/write, and control-plane send/receive handle `EINTR`. All created listener/client/epoll/eventfd descriptors use close-on-exec.

## Invalid protocol

Body lengths below the fixed header or above the compile-time maximum close the connection before allocation. Incomplete frames remain in the input buffer. Complete frames preceding a partial tail are emitted and the tail is retained.

The runtime `max_payload_size` is also checked after decode. A stricter runtime value therefore rejects the connection, although the compile-time guard remains the earliest allocation bound.

## Stale Worker response after fd reuse

Every accepted connection receives a unique conn_id. A response or forced-close record applies only when both fd and conn_id match the current map entry. An old response is discarded rather than sent to the new owner of a reused fd.

## Shutdown deadline

The Gateway stops accepting and reading new work, but drains tasks already admitted to both input queues. The last producer across normal and AUTH Workers stops the Response Queue. At the deadline all three queues are aborted and remaining connections close. Startup reserves at least two control-plane timeout budgets plus 100 ms; AUTH calls begun during DRAINING are additionally capped by the shutdown deadline, and the reporter cannot start its clients call after shutdown begins. Synchronous DNS remains the documented exception. The guarantee is bounded best effort, not “every client always receives every response.”
