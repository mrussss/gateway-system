# v2 Redis Schema Contract

```text
token:{client_id}             Hash: digest, generation, created_at, updated_at, disabled
token:index                   Set: client_id
gateway:status:{gateway_id}   Hash: complete latest metrics snapshot; TTL 5m
gateway:clients:{gateway_id}  String(JSON): complete latest client snapshot; TTL 60s
gateway:index                 ZSET: score=last metrics Unix timestamp, member=gateway_id
config:active                 Hash: complete runtime configuration snapshot
auth:failures:{identity}      String(counter) with TTL
```

Docker and Kubernetes use Redis. `MemoryStore` exists only for unit tests and
explicit local development.

## Token invariants

Only HMAC-SHA256 digests are stored; plaintext tokens and the pepper are never
persisted. Creation atomically adds the record and index member. Rotation uses
Lua CAS on `generation`, replaces the digest, re-enables the record, and updates
the timestamp as one atomic operation. Listing exposes neither `digest` nor
plaintext.

## Gateway state invariants

Metrics reports refresh the status TTL and update `gateway:index`. Client
reports refresh the client snapshot TTL but do not refresh gateway online time.
Online status is derived from the latest metrics report and the online window;
retention additionally depends on key TTL. Listing removes index members whose
status keys have expired.

Reporting operations use Redis Pipeline to reduce network round trips.
**Pipeline is not a transaction and does not provide atomicity.** A partial
report failure is observable and is repaired by a later complete snapshot.

## Configuration invariants

`config:active` contains exactly one full snapshot: `version`,
`max_payload_size`, `max_connections_per_client`,
`max_requests_per_client_per_second`, `slow_client_output_limit`, `log_level`,
and `request_queue_capacity_display`. Initialization creates version 1 only
when the key does not exist.

Update uses one Lua CAS operation: read and compare the expected version, derive
a strictly greater version, write every field, and return the new version.
Conflict or script failure does not modify the prior snapshot.

## AUTH failure limiter

Lua combines the first `INCR` and `PEXPIRE` so a new failure counter cannot be
left without an expiry. Only confirmed missing, disabled, or mismatched
credentials increment it. Successful AUTH clears it; Redis/network failure does
not turn into credential denial and does not create a misleading failure count.
