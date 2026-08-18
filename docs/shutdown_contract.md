# v2 Shutdown and Kubernetes Drain Contract

## C++ gateway

The lifecycle is `RUNNING -> DRAINING -> STOPPED`. SIGINT, SIGTERM, and
`TcpServer::stop()` are idempotent, including the duplicate signal sequence
caused by a Kubernetes preStop hook followed by normal pod termination.

Entering DRAINING immediately removes readiness, closes the listener, stops new
connections and request decoding, and prevents new queue writes. Work accepted
before the boundary, generated responses, and output buffers drain best-effort
until the configured deadline. Remaining work and connections are explicitly
aborted at that deadline.

The Reactor remains the sole owner of sockets and connection state. Workers
exchange values tagged with `fd + conn_id`; late responses are discarded rather
than delivered to reused file descriptors. AUTH work uses the earlier of its
HTTP deadline and the server shutdown deadline. Background reporting does not
start a second request after leaving RUNNING. Synchronous DNS resolution is the
documented exception to the socket deadline.

## Go control plane

The process uses signal-aware `http.Server.Shutdown`, stops accepting new HTTP
requests, gives in-flight handlers a bounded completion window, and then closes
the Store. Redis failure affects readiness, never liveness.

## Kubernetes ordering

Gateway pod termination uses this sequence:

```text
preStop sends SIGTERM to PID 1
-> gateway enters DRAINING and closes its listener
-> TCP readiness fails and EndpointSlice propagation is allowed to settle
-> Kubernetes sends the normal termination signal (safe duplicate)
-> accepted work drains until the gateway deadline
-> process exits, or Kubernetes enforces the pod grace period
```

The checked demonstration budget is a 3-second propagation wait, a 20-second
gateway shutdown timeout, and `terminationGracePeriodSeconds: 30`. The invariant
is:

```text
terminationGracePeriodSeconds
  > preStop propagation wait + gateway shutdown timeout + safety margin
```

Gateway startup/readiness probes use TCP port 9000. Liveness must check process
liveness separately so DRAINING does not trigger a competing restart. Control
plane startup/liveness use `/health/live`; readiness uses `/health/ready`.

The guarantee is bounded best-effort completion for already accepted work. The
system does not migrate existing TCP connections, promise completion under every
fault, or claim zero client reconnects during replacement.
