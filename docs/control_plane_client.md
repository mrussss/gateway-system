# C++ ControlPlaneClient

ControlPlaneClient is a narrow internal HTTP client for the two calls needed by
the data plane:

- POST /auth/check from the dedicated AUTH Worker;
- GET /config from the background runtime-config puller.

HttpResponseParser incrementally enforces one valid Content-Length, bounded
headers/body, no unsupported transfer encoding and complete JSON framing.
SocketDeadline owns non-blocking connect/send/receive with one absolute
deadline. HTTP failure, invalid JSON and stale config versions are fail-closed
or last-known-good paths as appropriate.

Metrics-report, online-client report and fleet APIs are intentionally absent.
