# Gateway System

`gateway-system` is a backend infrastructure practice project built around a C++ TCP gateway and a Go control plane.

The C++ gateway is the data plane. It accepts TCP long connections, parses a custom binary protocol, dispatches business requests, and reports runtime state. The Go service is the control plane. It provides HTTP APIs for auth, metrics, gateway status, online clients, and config reload.

## Architecture

```text
C++ Gateway  <---- HTTP ---->  Go Control Plane
     |
     | TCP long connection / custom protocol
     |
 Client / benchmark scripts
```

## Directory Layout

```text
gateway-system/
|-- cpp-gateway/          # C++ epoll TCP gateway
|-- go-control-plane/     # Go HTTP control plane
|-- scripts/              # Project-level scripts
|-- docs/                 # Project docs
`-- README.md
```

## Features

- C++17 TCP gateway based on Linux socket and epoll.
- Custom packet codec with sticky-packet and half-packet handling.
- Worker-thread request dispatch.
- Go HTTP control plane built with standard `net/http`.
- Client auth through `POST /auth/check`.
- Gateway metrics reporting through `POST /metrics/report`.
- Gateway status query through `GET /gateway/status`.
- Online client reporting through `POST /clients/report`.
- Online client query through `GET /clients`.
- Fake config reload through `POST /config/reload`.

## Run

Start the Go control plane:

```bash
cd go-control-plane
go run .
```

The control plane listens on `localhost:8080`.

Build and start the C++ gateway:

```bash
cd cpp-gateway
cmake -S . -B build
cmake --build build
./build/message_server
```

The gateway listens on TCP port `9000` and calls the Go control plane on `127.0.0.1:8080`.

Run both services with Docker Compose:

```bash
docker compose up --build
```

## Test

Go unit tests:

```bash
cd go-control-plane
go test ./...
```

C++ build test:

```bash
cd cpp-gateway
cmake -S . -B build
cmake --build build
```

Existing gateway protocol scripts are under `cpp-gateway/scripts/`.

## HTTP API

### Health

```bash
curl http://localhost:8080/health
```

Response:

```json
{"status":"ok"}
```

### Auth Check

```bash
curl -X POST http://localhost:8080/auth/check \
  -H "Content-Type: application/json" \
  -d '{"client_id":"client_001","token":"test-token"}'
```

Allowed response:

```json
{"allowed":true,"reason":"ok"}
```

Rejected response:

```json
{"allowed":false,"reason":"invalid token"}
```

### Metrics Report

```bash
curl -X POST http://localhost:8080/metrics/report \
  -H "Content-Type: application/json" \
  -d '{"gateway_id":"gateway-001","active_connections":12,"total_messages":3456,"bytes_in":102400,"bytes_out":204800,"error_count":3,"timestamp":1710000000}'
```

### Gateway Status

```bash
curl http://localhost:8080/gateway/status
```

Example response:

```json
{
  "gateway_id": "gateway-001",
  "active_connections": 12,
  "total_messages": 3456,
  "bytes_in": 102400,
  "bytes_out": 204800,
  "error_count": 3,
  "last_report_time": "2024-03-09T16:00:00Z"
}
```

### Clients Report

```bash
curl -X POST http://localhost:8080/clients/report \
  -H "Content-Type: application/json" \
  -d '{"gateway_id":"gateway-001","clients":[{"client_id":"client_001","remote_addr":"127.0.0.1:50001","connected_at":"2026-06-08T12:00:00Z"}]}'
```

### Clients Query

```bash
curl http://localhost:8080/clients
```

Example response:

```json
[
  {
    "client_id": "client_001",
    "remote_addr": "127.0.0.1:50001",
    "connected_at": "2026-06-08T12:00:00Z"
  }
]
```

### Config Reload

```bash
curl -X POST http://localhost:8080/config/reload
```

Response:

```json
{"success":true,"message":"config reload triggered"}
```

## Current Limits

- Auth currently uses a hardcoded token: `test-token`.
- Runtime state is stored in Go process memory.
- The C++ gateway currently identifies accepted clients as `client_<conn_id>` until a real login/auth payload is added to the custom protocol.
- Docker Compose and external storage are planned but intentionally not included in the first working version.

## Roadmap

- Move tokens and runtime state to Redis or another storage backend.
- Add Docker Compose for one-command startup.
- Export Prometheus-format metrics.
- Add integration tests for C++ gateway to Go control plane communication.
- Add a minimal Go-rendered dashboard after the backend flow is stable.
