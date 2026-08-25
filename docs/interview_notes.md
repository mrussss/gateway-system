# Interview notes

## One-sentence summary

This is a Linux C++ authenticated long-connection gateway whose depth is in
Reactor ownership, bounded concurrency, overload behavior, dependency
isolation, versioned configuration and graceful shutdown.

## Core questions

- Why does one Reactor own sockets? To make epoll interest, close and output
  buffer lifetime single-owner operations.
- Why bounded queues? Backpressure must be explicit rather than an unbounded
  memory promise.
- Why a separate AUTH queue? Go/Redis latency must not starve ordinary TCP work.
- Why fd plus conn_id? The kernel can reuse an integer fd after a disconnect.
- Why eventfd? Worker responses wake an otherwise blocking epoll wait without
  periodic polling.
- How are partial writes handled? An output buffer and offset retain the
  unsent suffix and use EPOLLOUT.
- What happens when Redis fails? Readiness fails and AUTH fails closed; an
  established data connection can continue ordinary work.
- How is config safe? Go validates and performs Lua CAS; C++ validates and
  swaps only a strictly newer immutable snapshot.
- How is shutdown bounded? RUNNING becomes DRAINING, the listener and queues
  stop accepting new work, output is flushed until a deadline, then pending
  work is aborted.

## Evidence

CTest, ASan/UBSan, Go race/vet, real Redis contracts, Compose smoke, Redis
recovery, fd reuse/failure tests, slow-reader/overload tests and benchmark
results are the evidence to discuss. The project deliberately does not claim
Kubernetes orchestration, Prometheus operations, fleet management, client
aggregation or HA Redis.
