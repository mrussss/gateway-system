# Docker/Redis Release benchmark — GitHub Actions 32160645066

## Environment, build, and command

- Run: `https://github.com/mrussss/gateway-system/actions/runs/32160645066`
- Candidate: `89efbedcf542496e8b62bcfe7a5dd05b50cb7ff7`.
- Host: GitHub-hosted Azure Linux runner, 4 logical x86-64 CPUs, 15 GiB RAM,
  kernel `6.17.0-1022-azure`.
- Toolchain: GCC 13.3.0, CMake 3.31.6, Go 1.24.13, Python 3.12.3,
  Docker 28.0.4.
- Runtime: Release application containers, non-root/read-only Compose services,
  Redis 7 Alpine backend, loopback host-to-container traffic.
- Client model: one Python thread and persistent authenticated connection per
  client; 20 ECHO requests per client; setup concurrency 16.
- Exact command: `scripts/benchmark_matrix.sh` with its checked-in defaults.

The raw environment is in `environment.txt`. Every JSON file contains complete
parameters, request and AUTH samples, before/after process and Gateway STATS,
and benchmark-interval Redis histogram deltas.

## Results

Scenario columns are `Workers / Queue capacity / clients / payload bytes`; the
two slow rows use 100 clients, 4096-byte payloads, 10% slow readers, and a 50 ms
read delay.

| Scenario | Success | QPS | Request P99 | AUTH P99 | CPU | Peak RSS | Redis avg | Request/Response reject delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 / 64 / 1 / 128 | 20/20 | 7278.8 | 0.18 ms | 1.21 ms | 0.0%* | 4516 KiB | 0.116 ms | 0/0 |
| 1 / 64 / 1 / 4096 | 20/20 | 5757.7 | 0.22 ms | 1.26 ms | 0.0%* | 4564 KiB | 0.104 ms | 0/0 |
| 1 / 64 / 10 / 128 | 200/200 | 10119.6 | 2.54 ms | 5.29 ms | 50.6% | 4588 KiB | 0.205 ms | 0/0 |
| 1 / 64 / 10 / 4096 | 200/200 | 8887.8 | 2.89 ms | 4.36 ms | 44.4% | 4724 KiB | 0.149 ms | 0/0 |
| 1 / 64 / 100 / 128 | 2000/2000 | 9998.4 | 26.88 ms | 10.10 ms | 50.0% | 4756 KiB | 0.205 ms | 0/0 |
| 1 / 64 / 100 / 4096 | 2000/2000 | 9779.9 | 27.13 ms | 7.24 ms | 58.7% | 5548 KiB | 0.153 ms | 0/0 |
| 1 / 64 / 500 / 128 | 10000/10000 | 9536.2 | 154.64 ms | 22.84 ms | 49.6% | 5548 KiB | 0.182 ms | 0/0 |
| 1 / 64 / 500 / 4096 | 9974/10000 | 9100.5 | 160.89 ms | 11.75 ms | 55.7% | 11624 KiB | 0.183 ms | 0/7 |
| 1 / 64 / slow | 2000/2000 | 1926.4 | 53.50 ms | 13.61 ms | 10.6% | 11624 KiB | 0.257 ms | 0/0 |
| 4 / 4096 / 1 / 128 | 20/20 | 6643.6 | 0.18 ms | 1.26 ms | 0.0%* | 4776 KiB | 0.105 ms | 0/0 |
| 4 / 4096 / 1 / 4096 | 20/20 | 6222.1 | 0.21 ms | 1.21 ms | 0.0%* | 4876 KiB | 0.102 ms | 0/0 |
| 4 / 4096 / 10 / 128 | 200/200 | 9676.1 | 2.28 ms | 4.77 ms | 48.4% | 4912 KiB | 0.195 ms | 0/0 |
| 4 / 4096 / 10 / 4096 | 200/200 | 8682.5 | 2.67 ms | 4.93 ms | 43.4% | 5096 KiB | 0.143 ms | 0/0 |
| 4 / 4096 / 100 / 128 | 2000/2000 | 9836.5 | 27.50 ms | 9.11 ms | 54.1% | 5120 KiB | 0.188 ms | 0/0 |
| 4 / 4096 / 100 / 4096 | 2000/2000 | 9323.6 | 29.02 ms | 13.60 ms | 60.6% | 6028 KiB | 0.201 ms | 0/0 |
| 4 / 4096 / 500 / 128 | 10000/10000 | 9040.8 | 158.88 ms | 9.56 ms | 54.2% | 6028 KiB | 0.167 ms | 0/0 |
| 4 / 4096 / 500 / 4096 | 10000/10000 | 8520.4 | 179.52 ms | 10.59 ms | 58.8% | 11884 KiB | 0.180 ms | 0/0 |
| 4 / 4096 / slow | 2000/2000 | 1866.9 | 58.63 ms | 7.89 ms | 12.1% | 11884 KiB | 0.201 ms | 0/0 |

`*` The 20-request interval used less than one scheduler tick. Redis latency
includes token creation and AUTH during setup; steady ECHO itself does not call
Redis.

The deliberately constrained one-Worker/64-slot profile exposed seven Response
Queue rejections at 500 clients with 4096-byte payloads. Closing affected
connections caused 26 of 10000 requests to fail; the raw per-client errors and
before/after counters preserve this rather than hiding it. The 4096-slot profile
completed the corresponding workload 10000/10000 without a rejection. Queue
peaks reached Request/Response 46/64 and 56/53 respectively.

## Limits

This is a short shared CI-runner comparison, not an SLO or capacity ceiling.
Python thread scheduling, Docker bridge/loopback, noisy-neighbor CPU, a single
Redis container, no TLS, and only 20 requests per connection all affect the
numbers. CPU is one-process CPU normalized to one logical core; RSS is sampled
from the host PID. Compare profiles within this run and reproduce on the target
hardware before using any absolute value for sizing.
