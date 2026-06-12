# Go Control Plane

Minimal HTTP control plane for the C++ gateway.

## Run

```bash
cd go-control-plane
go run .
```

The service listens on `:8080`.

For Docker Compose runs, the control plane uses Redis via:

- `STORE_BACKEND=redis`
- `REDIS_ADDR=redis:6379`

Local tests can continue using the in-process `MemoryStore`.

## Test

```bash
cd go-control-plane
go test ./...
```

## Curl

Health check:

```bash
curl http://localhost:8080/health
```

Valid auth request:

```bash
curl -X POST http://localhost:8080/tokens \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"test-token"}'
```

```bash
curl -X POST http://localhost:8080/auth/check \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"test-token"}'
```

Invalid auth request:

```bash
curl -X POST http://localhost:8080/auth/check \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"bad-token"}'
```

Report gateway metrics:

```bash
curl -X POST http://localhost:8080/metrics/report \
  -H "Content-Type: application/json" \
  -d '{"gateway_id":"gateway-001","active_connections":12,"total_messages":3456,"bytes_in":102400,"bytes_out":204800,"error_count":3,"timestamp":1710000000}'
```

Query gateway status:

```bash
curl http://localhost:8080/gateway/status
```

Report online clients:

```bash
curl -X POST http://localhost:8080/clients/report \
  -H "Content-Type: application/json" \
  -d '{"gateway_id":"gateway-001","clients":[{"client_id":"client_001","remote_addr":"127.0.0.1:50001","connected_at":"2026-06-08T12:00:00Z"}]}'
```

Query online clients:

```bash
curl http://localhost:8080/clients
```

List registered token owners:

```bash
curl http://localhost:8080/tokens
```

Delete a registered token:

```bash
curl -X DELETE http://localhost:8080/tokens/client_001
```

Read runtime config:

```bash
curl http://localhost:8080/config
```

Update runtime config:

```bash
curl -X POST http://localhost:8080/config \
  -H "Content-Type: application/json" \
  -d '{
    "auth_timeout_ms":1000,
    "max_payload_size":4194314,
    "max_connections_per_client":2,
    "max_requests_per_client_per_second":100,
    "fail_open":false
  }'
```

Trigger config reload:

```bash
curl -X POST http://localhost:8080/config/reload
```

## Notes

- `/auth/check` validates `client_id + token` through the configured store backend.
- `GET /tokens` only returns `client_id` values and does not expose token plaintext.
- Docker Compose defaults to Redis for tokens, runtime config, gateway status, and clients.
- `MemoryStore` remains available for local tests and non-Redis runs.
- `POST /config` replaces the full runtime config and increments `version`.
- `POST /config/reload` is a no-op that returns the current `version`.
- AUTH requires explicit token registration through `POST /tokens`.
