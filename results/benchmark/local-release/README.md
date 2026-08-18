# Local Release benchmark — 2026-08-19

## Environment and scope

- Host: WSL2 Linux on `LAPTOP-7Q69GP7V`, 32 logical CPUs, 15 GiB visible RAM.
- Kernel: `6.18.33.2-microsoft-standard-WSL2` x86_64.
- Toolchain: GCC 11.4.0, CMake 3.22.1, Go 1.22.12, Python 3.10.12.
- Gateway: local non-container CMake `Release` build; four normal workers;
  Request/Response Queue capacity 4096.
- Control plane: local Go process with `MemoryStore`; Redis latency observations
  are therefore zero by design.
- Client model: one Python thread and one persistent authenticated TCP
  connection per client, loopback networking, ECHO request/response.

The complete capture is
[`../../environment/20260819-local-wsl.txt`](../../environment/20260819-local-wsl.txt).
Docker was unavailable because Docker Desktop WSL integration was disabled;
`kubectl` was also unavailable. These numbers do not prove container, Redis, or
Kubernetes capacity.

## Commands

The services were started with:

```bash
(cd go-control-plane && APP_ENV=development STORE_BACKEND=memory go run ./cmd/control-plane)
cmake -S cpp-gateway -B cpp-gateway/build-release -DCMAKE_BUILD_TYPE=Release \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build cpp-gateway/build-release --parallel
APP_ENV=development GATEWAY_ID=benchmark-local WORKER_COUNT=4 \
  REQUEST_QUEUE_CAPACITY=4096 RESPONSE_QUEUE_CAPACITY=4096 \
  ./cpp-gateway/build-release/message_server
```

Each raw file was produced with the equivalent of:

```bash
CONTROL_PLANE_ADMIN_TOKEN='' python3 scripts/benchmark_tcp.py \
  --mode steady --build-mode Release-local-memory \
  --clients CLIENTS --requests-per-client REQUESTS --payload-size BYTES \
  --gateway-pid GATEWAY_PID --worker-count 4 \
  --request-queue-capacity 4096 --response-queue-capacity 4096 \
  --output results/benchmark/local-release/NAME.json
```

The slow-reader run additionally used `--slow-client-ratio 0.10
--slow-read-delay-ms 50`. The full-path run used `--mode full`, which includes
HTTP token creation, TCP connect, and AUTH in elapsed throughput.

## Raw results

| Mode | Clients × requests | Payload | Slow readers | Success | QPS | P50 | P95 | P99 | AUTH P99 | CPU | RSS delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| steady | 1 × 100 | 128 B | 0% | 100/100 | 5201.56 | 0.18 ms | 0.26 ms | 0.30 ms | 0.70 ms | 0.0%* | +20 KiB |
| steady | 10 × 100 | 128 B | 0% | 1000/1000 | 7198.55 | 1.31 ms | 2.23 ms | 2.77 ms | 4.01 ms | 35.99% | +16 KiB |
| steady | 100 × 50 | 128 B | 0% | 5000/5000 | 6448.45 | 14.41 ms | 26.00 ms | 31.96 ms | 41.08 ms | 29.66% | +40 KiB |
| steady | 500 × 20 | 128 B | 0% | 10000/10000 | 4903.89 | 15.77 ms | 33.79 ms | 49.44 ms | 25.62 ms | 31.88% | +44 KiB |
| steady | 100 × 20 | 4096 B | 0% | 2000/2000 | 5825.65 | 14.61 ms | 29.92 ms | 38.63 ms | 8.02 ms | 34.95% | +632 KiB |
| steady | 100 × 10 | 4096 B | 10% | 1000/1000 | 1764.04 | 15.40 ms | 50.36 ms | 68.90 ms | 6.71 ms | 14.11% | -464 KiB† |
| full | 10 × 20 | 128 B | 0% | 200/200 | 5882.51 | 1.09 ms | 1.83 ms | 2.23 ms | 2.77 ms | 29.41% | 0 KiB |

`*` The one-client interval consumed less than one scheduler tick, so `/proc`
reported a zero CPU delta. `†` RSS is an instantaneous before/after sample and
may fall after allocator reclamation; peak RSS in the raw record did not fall.

No Request or Response Queue rejection occurred. Across this process lifetime,
the observed Request Queue peak reached 7 and the Response Queue peak reached
10. Every number above is derived from the linked JSON files in this directory;
the per-client latency arrays remain in those files.

## Limitations

This is a short loopback comparison run on one shared machine without TLS,
network delay, container isolation, or Redis. Python thread scheduling affects
high-concurrency percentiles. Process CPU is normalized to one logical CPU and
instantaneous RSS is not an allocator profile. The run does not establish a
production SLO or maximum connection count. Use `scripts/benchmark_matrix.sh`
in a Docker-enabled environment to reproduce both one-worker/small-queue and
four-worker/large-queue profiles with Redis.
