# Redis Schema Contract

```text
token:{client_id}             Hash: digest, generation, created_at, updated_at, disabled
token:index                   Set
gateway:status:{gateway_id}   Hash
gateway:clients:{gateway_id}  String(JSON)
gateway:index                 ZSET: score=last metrics Unix timestamp, member=gateway_id
config:active                 Hash
auth:failures:{client_id}     String(counter) with TTL
```

Only HMAC token digests are stored; plaintext tokens and peppers are never
persisted. Gateway status has a five-minute TTL and client snapshots have a
60-second TTL. A gateway is offline after 30 seconds without a metrics report,
but its status is retained until the status TTL expires. Client snapshots never
refresh gateway online time.

Metrics reporting pipelines `HSET`, `EXPIRE`, and `ZADD`; it reduces round
trips and is not described as a transaction. Configuration and token rotation
use Lua compare-and-set for their atomic update boundaries.

AUTH failure recording uses Lua so `INCR` and the first failure's `PEXPIRE` are one atomic operation. Only confirmed missing, disabled, or mismatched credentials increment this key; successful authentication deletes it, while Redis and network failures leave it unchanged.
