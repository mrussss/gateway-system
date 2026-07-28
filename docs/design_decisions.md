# Design Decisions

## One Reactor

One Reactor makes connection ownership, fd reuse, close ordering, and shutdown auditable. Current measured throughput is sufficient for the project goal. Multi-Reactor sharding would add cross-Reactor routing and balancing before there is evidence it is needed.

## Workers return values, not socket operations

Workers may block on business/control-plane work. They return Response objects so socket state stays single-owner. This prevents concurrent writes and centralizes stale-response validation.

## fd plus conn_id

Linux can reuse a small fd immediately after close. An fd alone is not an identity across asynchronous work. A monotonic conn_id turns response application into an exact generation check.

## eventfd instead of timeout polling

The old 100ms epoll timeout appeared directly in low-load latency. Eventfd integrates cross-thread wakeup with epoll, coalesces notifications safely, supports signal-triggered stop, and lets normal operation wait indefinitely without sacrificing progress.

## Bounded queues and explicit push results

An unbounded queue converts sustained overload into eventual OOM. Fixed capacity creates a measurable rejection point. `OK/FULL/STOPPED` prevents shutdown and overload failures from being silently ignored.

## Deadline-bounded shutdown

Infinite drain lets a slow client prevent deployment or process termination forever. Immediate close loses accepted work unnecessarily. The state machine drains best-effort within a fixed deadline and documents the exact acceptance boundary.

## Synchronous AUTH HTTP remains in Workers

The call is not ideal for very high scale, but it never blocks the Reactor and keeps the current scope understandable. An async HTTP client is deferred until measurements show Worker occupancy is the limiting factor.

## No Kafka, dashboard, or extra database

Kubernetes and Prometheus are fixed v2 validation and observability scope. Kafka,
a dashboard, and an extra database would increase breadth while weakening time
spent on correctness evidence and ownership.
