# Shutdown Contract

The gateway state machine is `RUNNING -> DRAINING -> STOPPED`. SIGINT, SIGTERM,
and `stop()` are idempotent. Entering DRAINING removes the readiness file,
closes the listener, stops new decoding and request-queue writes, then drains
accepted work, responses, and output buffers until the configured deadline.
Remaining connections are closed at the deadline.

The Reactor remains the sole owner of socket operations and connection state;
workers only exchange immutable request/response values tagged by `fd + conn_id`.
The system provides best-effort completion only for accepted work before the
deadline. It does not guarantee completion under every fault or transparent TCP
connection migration during process replacement.

The control plane uses signal-aware `http.Server.Shutdown`, waits for in-flight
HTTP work until its deadline, then closes the Store.
