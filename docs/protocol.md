# Protocol

## Packet Format

Each TCP packet is length-prefixed:

```text
uint32 body_length
uint8  version
uint8  message_type
uint64 request_id
bytes  payload
```

Rules:

- `body_length` does not include the 4-byte length field itself
- `body_length` includes `version + message_type + request_id + payload`
- multi-byte integers use network byte order
- the fixed header size after the length prefix is 10 bytes

## Message Types

```text
1  PING
2  ECHO
3  LOG_PUSH
4  STATS
5  PONG
6  ECHO_RESP
7  ERROR_RESP
8  LOG_ACK
9  STATS_RESP
10 AUTH
11 AUTH_RESP
```

## Request and Response Summary

`PING`

- request payload: empty
- response type: `PONG`
- response payload: JSON with `message: "pong"`

`ECHO`

- request payload: arbitrary bytes
- response type: `ECHO_RESP`
- response payload: original bytes

`LOG_PUSH`

- request payload: JSON object with `level`, `service`, `message`
- response type: `LOG_ACK` on success
- error response: `ERROR_RESP` for invalid JSON, missing fields, or oversized payload

`STATS`

- request payload: empty
- response type: `STATS_RESP`
- response payload: JSON counters for requests, errors, bytes, active connections, and queue backlog

`AUTH`

- request payload: JSON object with `client_id` and `token`
- response type: `AUTH_RESP` on success
- response type: `AUTH_RESP` with `allowed=false` when the per-process connection limit is exceeded after auth validation
- connection close on malformed or rejected auth

## AUTH Rules

New connections start unauthenticated.

Before auth succeeds:

- only `AUTH` is allowed
- any business message closes the connection
- a second message while auth is pending closes the connection

`AUTH` request shape:

```json
{"client_id":"client_001","token":"test-token"}
```

Success path:

- payload must be valid JSON
- `client_id` must be a string
- `token` must be a string
- the Go control plane must allow the token
- the connection is then marked authenticated
- successful `AUTH` returns `{"allowed":true,"reason":"ok"}`

Failure path:

- invalid JSON closes the connection
- missing `client_id` or `token` closes the connection
- invalid field types close the connection
- rejected token closes the connection
- when the control plane is unreachable or returns a store error, auth also fails closed

AUTH pending behavior:

- the first `AUTH` request is queued to a worker thread
- the connection stays unauthenticated while that worker request is in flight
- any second packet that arrives before the auth result is applied closes the connection

After auth succeeds:

- business messages are allowed
- a repeated `AUTH` request returns `ERROR_RESP` with `{"status":400,"message":"already authenticated"}`
- `/clients` reports the authenticated `client_id`

Connection limit behavior:

- after a successful auth result is ready, the gateway counts other authenticated connections for the same `client_id` in the local process
- if that count would exceed `max_connections_per_client`, the gateway returns `AUTH_RESP` with `{"allowed":false,"reason":"max connections exceeded"}`
- that response is followed by connection close

Rate-limit behavior:

- after auth succeeds, the gateway enforces `max_requests_per_client_per_second`
- when the current per-second window is exceeded, the gateway returns `ERROR_RESP`
- the rate-limit payload is `{"status":429,"message":"rate limited"}`
- the connection stays open

## Error Handling

The protocol currently uses two styles of failure:

- `ERROR_RESP` for business-level errors such as duplicate `AUTH` or invalid business payloads
- direct connection close for malformed protocol frames and rejected auth
- `AUTH_RESP` with `allowed=false` for connection-limit rejection after otherwise valid auth

## Test Coverage

Current repo-level protocol tests cover:

- valid `AUTH`
- valid `PING`, `ECHO`, `LOG_PUSH`, `STATS`
- half-packet handling
- sticky-packet handling
- invalid length handling
- unauthenticated request rejection
- invalid auth JSON
- missing auth fields
- invalid auth field types
- auth pending second-request rejection
- duplicate auth rejection
- per-process max connection rejection
- per-process rate limiting
- `/clients` visibility for authenticated clients only
