# Testing

## CTest matrix

| Test | Coverage |
| --- | --- |
| `protocol_codec_test` | complete/half/sticky frames, empty/max payload, invalid lengths, encode/decode identity, retained tail |
| `block_queue_test` | FIFO, full/stopped results, drain-after-stop, MPMC behavior, peak/capacity invariant |
| `runtime_config_test` | valid/invalid parsing, atomic retention, monotonic versions, failed fetch retention |
| `reactor_notifier_test` | epoll wakeup and coalesced multi-Worker eventfd writes |
| `server_stop_test` | concurrent/repeated programmatic `stop()` idempotence |
| `graceful_shutdown_test` | real process/signals, queue drain, overload 503, repeated stop, slow-client deadline |

Run:

```bash
ctest --test-dir cpp-gateway/build --output-on-failure
```

The integration test starts an in-process fake HTTP control plane and the built `message_server`; it does not require Redis or Docker.

## Sanitizers and strict build

CI compiles with `-Wall -Wextra -Wpedantic -Wformat=2 -Werror`. A separate build runs all CTests with AddressSanitizer and UndefinedBehaviorSanitizer. The fast CI also runs `go test`, `go test -race`, `go vet`, Compose validation, shell syntax, and Python byte compilation.

## Docker smoke

`bash scripts/smoke_test.sh` builds Redis, Go, and C++ containers, checks health/liveness/status APIs, and runs the full TCP protocol suite. GitHub exposes this as a manual workflow to keep ordinary pushes fast.

The protocol suite covers AUTH state, half/sticky packets, malformed lengths, rate and connection limits, client reporting, repeated connection lifecycle, and concurrent auth/echo.

## Failure evidence

- Go `errorStore` tests model Redis/store failures.
- `runtime_config_test` proves a failed fetch cannot mutate the active config object.
- graceful shutdown integration uses delayed auth to create a real queue backlog.
- overload integration runs with Request Queue capacity 1 and verifies both admitted AUTH and explicit 503 rejection.
- slow-client integration reduces the receive buffer, generates large responses, and verifies the configured shutdown deadline is exercised.
- deep-queue deadline integration uses slow AUTH work and proves not-yet-started requests are aborted instead of extending shutdown indefinitely.
- control-plane outage integration proves existing authenticated ECHO remains available, new AUTH fails closed, and reporting/config errors do not terminate the Gateway.
