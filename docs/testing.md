# Testing

The test suite protects the final ownership boundary.

## C++

CTest covers protocol framing, bounded queues, eventfd notification, runtime
config parsing/versioning, AUTH tasks, the control-plane HTTP parser/deadline,
fd plus conn_id stale-response safety, peer half-close behavior and graceful
shutdown. The Python black-box suite covers single/sticky/AUTH half-close,
truncated EOF input, AUTH isolation, queue overload, slow readers, failure
recovery and bounded drain.

Run normal and sanitized builds:

    cmake -S cpp-gateway -B cpp-gateway/build -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_WARNINGS_AS_ERRORS=ON
    cmake --build cpp-gateway/build --parallel
    ctest --test-dir cpp-gateway/build --output-on-failure

## Go

Go tests cover token digest storage, lifecycle and generation CAS, AUTH failure
TTL/atomicity/fail-closed behavior, config CAS, HTTP foundation, readiness and
context/deadline shutdown.

    cd go-control-plane
    go test ./...
    go test -race ./...
    go vet ./...

## Redis and Compose

With a real Redis at REDIS_TEST_ADDR, integration tests verify token lifecycle,
AUTH failure TTL/atomic increments, config Lua CAS conflicts and outage
semantics. The full release gate also runs Compose token/AUTH/config/TCP smoke,
Redis pause/recovery and the authenticated C++ benchmark.

The suite intentionally contains no Kubernetes, Prometheus, fleet or online
client aggregation contract.
