# AUTH Failure Semantics

`Allowed` means Go returned a valid 2xx JSON body with `allowed=true`. `Denied` means the same trusted contract returned `allowed=false`. `Unavailable` covers DNS, connect, deadline, send, receive, HTTP framing/status, JSON, Go, and Redis failures. All non-Allowed outcomes fail closed.

Go returns stable business codes under HTTP 200: `OK`, `INVALID_CREDENTIALS`, `TOKEN_DISABLED`, and `RATE_LIMITED`. Infrastructure failures return HTTP 503 `AUTH_UNAVAILABLE`. C++ queue overload returns local `AUTH_RESP/AUTH_OVERLOADED` and closes after writing.

Only confirmed token absence, mismatch, or disabled state increments `auth:failures:{client_id}`. Invalid JSON, gateway-token rejection, Redis failure, C++ queue overload, and HTTP failure do not. Redis uses atomic `INCR` plus first-failure expiry; success clears the counter. Defaults are five failures in sixty seconds and are startup-configurable.

AUTH saturation is contained by `AUTH_WORKER_COUNT` concurrent calls and `AUTH_QUEUE_CAPACITY` waiting tasks. A disconnected queued task is cancelled before start when possible. A race after that check remains safe because Reactor response application always validates both fd and conn_id.
