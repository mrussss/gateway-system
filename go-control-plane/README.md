# Go Control Plane

Minimal HTTP control plane for the C++ gateway.

## Run

```bash
cd go-control-plane
go run .
```

The service listens on `:8080`.

Set `ADMIN_TOKEN`, `GATEWAY_SHARED_TOKEN`, and `TOKEN_PEPPER` in deployed
environments. The examples below use `admin-secret` and `gateway-secret`.

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
  -H "Authorization: Bearer admin-secret" \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001"}'
```

The generated token is returned only by this create response. Save it as
`CLIENT_TOKEN` before making the gateway-internal auth request.

```bash
curl -X POST http://localhost:8080/auth/check \
  -H "X-Gateway-Token: gateway-secret" \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"'"$CLIENT_TOKEN"'"}'
```

Invalid auth request:

```bash
curl -X POST http://localhost:8080/auth/check \
  -H "X-Gateway-Token: gateway-secret" \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"bad-token"}'
```

Report gateway metrics:

```bash
curl -X POST http://localhost:8080/metrics/report \
  -H "X-Gateway-Token: gateway-secret" \
  -H "Content-Type: application/json" \
  -d '{"gateway_id":"gateway-001","gateway_boot_id":"boot-001","process_start_time":1710000000,"active_connections":12,"total_requests":3456,"bytes_in":102400,"bytes_out":204800,"error_count":3,"request_queue_capacity":4096,"request_queue_backlog":0,"request_queue_peak":12,"request_queue_rejected":0,"response_queue_capacity":4096,"response_queue_backlog":0,"response_queue_peak":12,"response_queue_rejected":0,"slow_client_closed":0,"stale_response_dropped":0,"auth_success":10,"auth_failure":1,"runtime_config_version":1,"server_state":"RUNNING","timestamp":1710000000}'
```

Query gateway status:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/gateway/status
```

List reported gateways:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/gateways
```

Query one gateway status:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/gateways/gateway-001/status
```

Report online clients:

```bash
curl -X POST http://localhost:8080/clients/report \
  -H "X-Gateway-Token: gateway-secret" \
  -H "Content-Type: application/json" \
  -d '{"gateway_id":"gateway-001","clients":[{"client_id":"client_001","remote_addr":"127.0.0.1:50001","connected_at":"2026-06-08T12:00:00Z"}]}'
```

Query online clients:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/clients
```

Query one gateway's online clients:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/gateways/gateway-001/clients
```

List registered token owners:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/tokens
```

Delete a registered token:

```bash
curl -X DELETE http://localhost:8080/tokens/client_001 \
  -H "Authorization: Bearer admin-secret"
```

Read runtime config:

```bash
curl -H "Authorization: Bearer admin-secret" http://localhost:8080/config
```

Update runtime config:

```bash
curl -X PUT http://localhost:8080/config \
  -H "Authorization: Bearer admin-secret" \
  -H 'If-Match: "1"' \
  -H "Content-Type: application/json" \
  -d '{
    "max_payload_size":1048576,
    "max_connections_per_client":2,
    "max_requests_per_client_per_second":100,
    "slow_client_output_limit":8388608,
    "log_level":"INFO"
  }'
```

## Notes

- `/auth/check` validates `client_id + token` through the configured store backend.
- `GET /tokens` returns metadata and never exposes a token secret or digest.
- Docker Compose defaults to Redis for tokens, runtime config, gateway status, and clients.
- Multi-gateway APIs are available through `/gateways`, `/gateways/{gateway_id}/status`, and `/gateways/{gateway_id}/clients`.
- Legacy `GET /gateway/status` and `GET /clients` remain available and return the latest reported data.
- `MemoryStore` remains available for local tests and non-Redis runs.
- `PUT /config` requires the current ETag in `If-Match` and increments `version` atomically.
- Token disable and rotate affect new AUTH attempts; existing authenticated connections stay active.
- AUTH requires explicit token creation through `POST /tokens`.
