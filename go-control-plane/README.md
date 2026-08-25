# Go Control Plane

The Go service is intentionally small: standard-library HTTP, secure token
lifecycle, AUTH decisions with Redis failure TTL, versioned runtime config and
basic health/shutdown.

## Run

```bash
cd go-control-plane
APP_ENV=development STORE_BACKEND=memory go run ./cmd/control-plane
```

Production-like environments require
`CONTROL_PLANE_ADMIN_TOKEN`, `GATEWAY_SHARED_TOKEN` and `TOKEN_PEPPER`.
Compose uses `STORE_BACKEND=redis` and `REDIS_ADDR=redis:6379`.

## API examples

Create a token:

```bash
curl -X POST http://localhost:8080/tokens \
  -H "Authorization: Bearer admin-secret" \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001"}'
```

Check it from the gateway:

```bash
curl -X POST http://localhost:8080/auth/check \
  -H "X-Gateway-Token: gateway-secret" \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"<one-time-secret>"}'
```

Read and update config with the returned ETag:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/config
curl -X PUT http://localhost:8080/config \
  -H "Authorization: Bearer admin-secret" -H 'If-Match: "1"' \
  -H "Content-Type: application/json" \
  -d '{"max_payload_size":1048576,"max_connections_per_client":2,"max_requests_per_client_per_second":100,"slow_client_output_limit":8388608,"log_level":"INFO"}'
```

## Guarantees

- `GET /tokens` returns metadata only; Redis and MemoryStore never persist
  plaintext tokens.
- AUTH failures are counted atomically with a TTL, cleared on success, and
  return `AUTH_UNAVAILABLE` when the Store fails.
- Config updates use Redis Lua CAS and C++ applies only validated newer
  versions.
- `GET /health/ready` reflects Store reachability.
- Standard signal/context/deadline shutdown closes the HTTP server and Store.

Run the unit, race, vet and real-Redis checks with the repository release gate.
