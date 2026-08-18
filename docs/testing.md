# Testing

## CTest matrix

| Test | Coverage |
| --- | --- |
| `protocol_codec_test` | complete/half/sticky frames, empty/max payload, invalid lengths, encode/decode identity, retained tail |
| `block_queue_test` | FIFO, full/stopped results, drain-after-stop, MPMC behavior, peak/capacity invariant |
| `runtime_config_test` | valid/invalid parsing, atomic retention, monotonic versions, failed fetch retention |
| `reactor_notifier_test` | epoll wakeup and coalesced multi-Worker eventfd writes |
| `server_stop_test` | concurrent/repeated programmatic `stop()` idempotence |
| `control_plane_client_test` | strict framing, outcome mapping, shared deadline, high fd, size limits, CR/LF injection |
| `startup_config_test` | valid and fail-fast AUTH/HTTP startup settings |
| `auth_task_test` | AUTH-only task invariant and shared cancellation token |
| `graceful_shutdown_test` | real process/signals, dual-queue drain, AUTH bulkhead, cancellation, outage, slow-client deadline |

Run:

```bash
ctest --test-dir cpp-gateway/build --output-on-failure
```

The integration test starts an in-process fake HTTP control plane and the built `message_server`; it does not require Redis or Docker.

## Sanitizers and strict build

CI compiles with `-Wall -Wextra -Wpedantic -Wformat=2 -Werror`. A separate build runs all CTests with AddressSanitizer and UndefinedBehaviorSanitizer. The fast CI also runs `go test`, `go test -race`, `go vet`, real Redis integration contracts, Compose validation, shell syntax, and Python byte compilation.

## Docker smoke

`bash scripts/smoke_test.sh` builds Redis, Go, and C++ containers; proves the
application containers are non-root with read-only root filesystems; checks
live/ready, status and client APIs; parses required Prometheus series; checks
Redis snapshot TTLs; verifies token create/list/rotate/disable and config CAS;
then runs the full TCP protocol suite. Set `SMOKE_KEEP_STACK=1` to retain the
stack after a local run. GitHub exposes this as a manual workflow to keep
ordinary pushes fast.

The protocol suite covers AUTH state, half/sticky packets, malformed lengths, rate and connection limits, client reporting, repeated connection lifecycle, and concurrent auth/echo.

## Redis outage and recovery

`bash scripts/redis_recovery_test.sh` starts the Compose stack, pauses Redis,
and proves that control-plane liveness stays 200 while readiness becomes 503,
an established authenticated TCP connection continues ECHO, and a new AUTH
fails closed. It then unpauses Redis and verifies readiness, new AUTH, reporting,
and the previously active configuration version recover without rollback. Set
`RECOVERY_KEEP_STACK=1` to retain the stack.

## Kubernetes smoke and rolling update

After `bash scripts/k8s_deploy.sh`, run `bash scripts/k8s_smoke.sh` and
`bash scripts/k8s_rolling_update_test.sh`. The smoke test covers replica count,
security context, protocol behavior, Prometheus, and Redis TTL. The rolling test
keeps reconnecting ECHO traffic active while replacing both gateway pods,
checks that old endpoint IPs stop listening, waits for rollout success, and
requires DRAINING logs plus new-pod metrics. The client fails if its observed
outage exceeds ten seconds.

`python3 scripts/k8s_manifest_test.py` is the fast no-cluster contract check and
runs on every push. It is not a substitute for the cluster tests.

## Failure evidence

- Go `errorStore` tests model Redis/store failures.
- `runtime_config_test` proves a failed fetch cannot mutate the active config object.
- graceful shutdown integration uses delayed auth to create a real queue backlog.
- overload integration saturates a one-worker/one-slot AUTH Executor, verifies local rejection, and proves ordinary ECHO remains responsive.
- slow-client integration reduces the receive buffer, generates large responses, and verifies the configured shutdown deadline is exercised.
- cancellation integration proves a disconnected queued AUTH does not call Go.
- deep-queue deadline integration uses slow AUTH work and proves not-yet-started AUTH tasks are aborted instead of extending shutdown indefinitely.
- control-plane outage integration proves existing authenticated ECHO remains available, new AUTH fails closed, and reporting/config errors do not terminate the Gateway.
- one-slot Request and Response Queue black-box cases prove explicit 503 and
  connection-close policies; the response path exposes rejection counters.
- the fd-generation case resolves the accepted socket through Linux `/proc`,
  forces exact fd reuse while an old AUTH is in flight, and proves the stale
  response cannot affect the replacement connection.
- the Go shutdown test cancels the SIGTERM-owned server context while a handler
  is in flight and proves listener closure, request completion, and Store Close.
- the Compose Redis recovery test proves live/ready separation and automatic
  recovery against a real paused Redis process.

The complete required fault mapping is recorded in the
[local fault-injection report](../results/failures/20260819-local.md); its
Docker/Redis and Kubernetes rows were subsequently closed by the
[final CI release report](../results/release/20260819-ci.md), with raw artifacts
retained beside that report.
