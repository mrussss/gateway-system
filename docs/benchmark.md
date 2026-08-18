# Benchmark

`scripts/benchmark_tcp.py` exercises the current v2 token and AUTH contract. It
creates a one-time token for every client, authenticates one persistent TCP
connection per Python thread, and writes structured JSON containing raw
per-client latency samples and aggregate evidence.

## Measurements and modes

Every run records success/failure, QPS, request average/P50/P95/P99/Max, AUTH
latency, setup latency, payload, client count, slow-reader ratio, build mode,
Worker/Queue settings, and Gateway STATS before/after. With `--gateway-pid`, it
also samples local Linux CPU and RSS. The control-plane Prometheus histogram is
sampled before and after the workload to report benchmark-interval Redis
operation count and mean latency.

- `--mode steady` prepares and authenticates all clients before the timer. It
  isolates the authenticated TCP request/response path.
- `--mode full` includes token registration, TCP connect, and AUTH in elapsed
  throughput. Per-request latency still measures only the selected TCP request.

Example:

```bash
python3 scripts/benchmark_tcp.py \
  --mode steady \
  --build-mode Release-local \
  --clients 100 \
  --requests-per-client 50 \
  --payload-size 4096 \
  --gateway-pid "$(pgrep -n -x message_server)" \
  --worker-count 4 \
  --request-queue-capacity 4096 \
  --response-queue-capacity 4096 \
  --output results/benchmark/manual.json
```

The complete Docker/Redis matrix is reproducible with:

```bash
scripts/benchmark_matrix.sh
```

It builds Release containers and compares one Worker with 64-slot queues to
four Workers with 4096-slot queues for 1/10/100/500 clients, 128/4096-byte
payloads, and a 10% slow-reader case. Environment variables documented by
`--help` and in the script can shorten or retain a run.

## Current local reference

The 2026-08-19 Release/MemoryStore loopback run completed all seven scenarios
without a failed request. Selected steady-state results were:

| Clients × requests | Payload | Slow | Success | QPS | P50 | P95 | P99 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 × 100 | 128 B | 0% | 100/100 | 5201.56 | 0.18 ms | 0.26 ms | 0.30 ms |
| 10 × 100 | 128 B | 0% | 1000/1000 | 7198.55 | 1.31 ms | 2.23 ms | 2.77 ms |
| 100 × 50 | 128 B | 0% | 5000/5000 | 6448.45 | 14.41 ms | 26.00 ms | 31.96 ms |
| 500 × 20 | 128 B | 0% | 10000/10000 | 4903.89 | 15.77 ms | 33.79 ms | 49.44 ms |
| 100 × 20 | 4096 B | 0% | 2000/2000 | 5825.65 | 14.61 ms | 29.92 ms | 38.63 ms |
| 100 × 10 | 4096 B | 10% | 1000/1000 | 1764.04 | 15.40 ms | 50.36 ms | 68.90 ms |

No queue rejection occurred; process-lifetime peaks reached 7 Request and 10
Response entries. The exact environment, commands, CPU/RSS/AUTH values, raw
JSON, and limitations are in the [run report](../results/benchmark/local-release/README.md).

## Interpretation and limits

The historical engineering result remains removal of the deterministic roughly
100 ms response wait: successful Worker pushes now notify eventfd and wake
`epoll_wait` immediately. Absolute QPS is not a capacity guarantee.

These reference runs use loopback, one shared WSL machine, no TLS, a Python
client, and MemoryStore. Redis observations are zero because Redis was not the
active backend. The separate [Docker/Redis matrix](../results/benchmark/container-ci/README.md)
records 18 Release-container scenarios across Workers, Queue capacities,
payloads, 1/10/100/500 clients, slow readers, CPU/RSS, rejection counters, AUTH,
and Redis latency. Both remain comparison evidence rather than an SLO. Re-run on
target hardware before making capacity claims.
