# Connection lifecycle

The Reactor is the sole owner of connection lifetime, socket I/O, epoll
interest and close operations. Workers receive value objects and return
responses tagged with both `fd` and monotonic `conn_id`.

## Normal lifecycle

```text
OPEN
  │ readable frames
  ▼
WORKING ── Worker completion ──► OUTPUT_PENDING
  │                                  │
  └──────────── close/error ◄────────┘
```

Only work successfully pushed to a bounded Request or AUTH Queue increments
`in_flight_work`. A Worker response decrements it only after the Reactor has
validated the matching `fd + conn_id`. Local overload and validation
responses do not decrement the counter.

## Peer half-close

```text
OPEN
  │ EPOLLIN | EPOLLRDHUP or recv == 0
  ▼
READ_EOF
  ├── no EPOLLIN registration
  ├── complete frames already buffered are still admitted
  ├── truncated trailing input is logged and discarded
  ├── Worker responses remain writable
  └── close after in_flight_work == 0 and output is empty
```

The event order is deliberate: fatal `EPOLLERR` closes immediately; readable
bytes are drained first; `EPOLLRDHUP`/`EPOLLHUP` then marks local read EOF and
observes that the peer write side is closed; pending output is flushed
afterward. This prevents FIN from dropping an AUTH request, sticky requests or
a complete frame followed by a truncated tail.

## Shutdown interaction

`DRAINING` stops new accepts and new decoded work, but admitted work and
responses continue until the shutdown deadline. A peer half-close follows the
same close-after-drain rule. A connection is eligible for close only when
`closing || read_eof || DRAINING`, `in_flight_work == 0`, and output is empty.
Deadline expiry closes remaining descriptors to keep process shutdown bounded.

Response-queue rejection records also use the complete `(fd, conn_id)`
identity. A stale generation cannot overwrite or close a newer connection.
