# Go Control Plane

Minimal HTTP control plane for the C++ gateway.

## Run

```bash
cd go-control-plane
go run .
```

The service listens on `:8080`.

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
