# ControlPlaneClient Contract

`ControlPlaneClient` is a small synchronous client for fixed internal JSON endpoints. It is not a general HTTP implementation.

The implementation is split by ownership: `SocketDeadline` owns fd lifetime, DNS/connect, `poll`, send/receive, and deadline accounting; `HttpResponseParser` incrementally owns status/header parsing and `Content-Length` framing; `ControlPlaneClient` owns fixed endpoint request construction, JSON business mapping, and metrics. These are narrow internal components, not a reusable HTTP framework.

After synchronous `getaddrinfo` completes, every resolved address plus connect, send, and receive shares one `steady_clock` deadline from `CONTROL_PLANE_TIMEOUT_MS`. A caller may supply an earlier lifecycle deadline; AUTH tasks started during DRAINING use the earlier of the request and shutdown deadlines. Sockets remain `SOCK_NONBLOCK | SOCK_CLOEXEC`; readiness uses `poll`, connect completion uses `SO_ERROR`, partial sends use `MSG_NOSIGNAL`, and `EINTR` never resets the budget.

Responses must be HTTP/1.0 or HTTP/1.1, contain exactly one decimal `Content-Length`, and fit separate 16 KiB header and 1 MiB body limits. Header names are case-insensitive. Transfer-Encoding, Content-Encoding, Upgrade, missing/duplicate length, early EOF, extra received body bytes, malformed status/header syntax, non-2xx status, and invalid JSON fail explicitly.

The result layers are:

- `HttpError`: resolve/deadline/connect/send/receive/framing/status/JSON detail.
- `HttpResult`: HTTP status and fixed body.
- `AuthResult`: `Allowed`, `Denied`, or `Unavailable` plus safe reason metadata.

Tokens and authorization headers are never logged. Host, gateway token, method, and path reject CR/LF. Each operation opens a fresh connection and sends `Connection: close`; there is no keep-alive, pool, cache, chunk decoding, compression, TLS, HTTP/2, redirect following, or DNS cancellation.
