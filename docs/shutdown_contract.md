# Shutdown contract

Both processes have bounded graceful shutdown.

## C++ gateway

The state transition is RUNNING → DRAINING → STOPPED. Stop requests wake the
Reactor through eventfd. DRAINING closes the listener, stops accepting new
work, drains accepted normal/AUTH requests and responses, flushes output, and
aborts remaining queue items at the deadline.

## Go control plane

signal.NotifyContext cancels the root context. The server calls
http.Server.Shutdown with a finite deadline and then closes the Store. In-flight
requests may finish within the deadline; new requests are rejected after the
listener closes.

The contract is independent of any orchestrator or probe implementation.

The C++ control-plane socket deadline bounds non-blocking connect, send and
receive operations. Synchronous `getaddrinfo` resolution used by both AUTH
Worker calls and background configuration pulls is not interruptible by that
socket deadline and remains the explicit DNS lifecycle exception.
