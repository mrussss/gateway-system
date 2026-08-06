# C++ Gateway

The C++17 data plane is a single-Reactor Linux TCP server using edge-triggered epoll, non-blocking/CLOEXEC sockets, a custom length-prefixed protocol, bounded Worker queues, and eventfd wakeups.

## Build and test

```bash
cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Sanitizers:

```bash
cmake -S . -B build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON \
  -DGATEWAY_ENABLE_SANITIZERS=ON
cmake --build build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitized --output-on-failure
```

## Core invariants

- The Reactor exclusively owns accept/read/write/close and epoll state.
- Normal and AUTH Workers exchange value objects through separately bounded input queues and one bounded Response Queue.
- Only AUTH Workers call the strict synchronous control-plane client; ordinary Workers never wait on Go/Redis.
- Control-plane sockets stay non-blocking and use `poll` with one connect/send/receive deadline.
- Every asynchronous result is checked against `fd + conn_id`.
- A successful Response Queue push is followed by eventfd notification.
- Queue `FULL` and `STOPPED` results are handled explicitly.
- Output uses a write offset; a slow connection is capped at 8 MiB.
- SIGINT/SIGTERM enters deadline-bounded DRAINING rather than immediately closing admitted work.
- Per-request logs are DEBUG metadata only; payloads and AUTH tokens are never printed.

Startup controls include `APP_ENV` (default `production`), `CONTROL_PLANE_TIMEOUT_MS` (default 1000), `AUTH_WORKER_COUNT` (2), and `AUTH_QUEUE_CAPACITY` (32). `GATEWAY_SHARED_TOKEN` is mandatory outside explicit development mode. Invalid or out-of-range values fail startup.

The authoritative system docs are in the repository root: [architecture](../docs/architecture.md), [testing](../docs/testing.md), [shutdown](../docs/shutdown.md), [benchmark](../docs/benchmark.md), and [design decisions](../docs/design_decisions.md).
