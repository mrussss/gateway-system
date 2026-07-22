# Benchmark

`scripts/benchmark_tcp.py` reports success/failure, QPS, average, P50/P95/P99/Max, optional gateway CPU/RSS, and Request/Response Queue telemetry.

## Modes

- `--mode steady`: register tokens, connect, and authenticate all clients before starting the timer. This isolates the authenticated TCP data path.
- `--mode full`: include token registration, TCP connect, and AUTH in total elapsed time. Per-request latency still measures the selected business request.

Example:

```bash
python3 scripts/benchmark_tcp.py \
  --mode steady \
  --build-mode Release \
  --clients 10 \
  --requests-per-client 100 \
  --message ping \
  --gateway-pid "$(pgrep -n -x message_server)"
```

`--gateway-pid` works only when the gateway is a local Linux process. It samples `/proc`; omit it for Docker or remote targets.

## Eventfd result

Before this change, the Reactor called `epoll_wait(..., 100)` and drained the Response Queue only after epoll returned. The historical one-connection average was approximately 100ms. The current Worker path performs:

```text
successful response_queue.push
  → eventfd write
  → epoll_wait returns immediately
  → eventfd read until EAGAIN
  → Response Queue drain
```

Reference runs on 2026-07-22 used a local Release build, the in-memory Go control plane, ping requests, 4 workers, and queue capacities of 8192:

| Clients × requests | Success | QPS | Avg | P50 | P95 | P99 | Max | Gateway CPU | Peak RSS |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 × 100 | 100 | 2947.57 | 0.33ms | 0.28ms | 0.60ms | 0.82ms | 0.93ms | 29.48% | 3584 KiB |
| 10 × 100 | 1000 | 3699.42 | 2.64ms | 2.47ms | 4.77ms | 5.90ms | 7.98ms | 33.29% | 3840 KiB |
| 100 × 100 | 10000 | 4459.63 | 21.78ms | 20.54ms | 39.57ms | 49.77ms | 71.57ms | 32.56% | 3840 KiB |

No Request/Response Queue rejection occurred. Process-lifetime observed queue peaks were 1 at one client and 5 at 10/100 clients.

A 10-client × 20-request full-path Echo run completed 200/200 requests at 2513.29 QPS; request P50/P95/P99 were 2.57/5.11/6.91ms. Full-path QPS is not directly comparable with steady-state QPS because it includes HTTP token registration and AUTH setup.

## Interpretation and limits

The important result is removal of the deterministic ~100ms low-load wait, not an absolute QPS claim. These runs use loopback, a short request/response pattern, one machine, and no TLS. Python client scheduling affects higher-concurrency percentiles. Re-run on the target machine and record CPU model, kernel, compiler, build flags, payload, process placement, and control-plane backend before publishing other numbers.

Useful scenarios are 1/10/100/500 clients, slow readers, and deliberately small queue capacities. Overload tests should report rejection counters rather than hiding failed requests.
