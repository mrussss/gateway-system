# Benchmark

The benchmark exercises authenticated TCP throughput and latency across a
controlled worker/request-queue matrix, payload sizes, concurrent clients and
slow readers. It records request success/failure accounting, latency
percentiles, AUTH latency, Gateway `STATS` before/after snapshots and optional
Linux process CPU/RSS samples.

The final matrix keeps the response queue and AUTH controls fixed:

| Profile | Workers | Request Queue | Response Queue | AUTH Workers | AUTH Queue |
| --- | ---: | ---: | ---: | ---: | ---: |
| `workers1-q64` | 1 | 64 | 4096 | 2 | 32 |
| `workers1-q4096` | 1 | 4096 | 4096 | 2 | 32 |
| `workers4-q64` | 4 | 64 | 4096 | 2 | 32 |
| `workers4-q4096` | 4 | 4096 | 4096 | 2 | 32 |

Each profile covers 1/10/100/500 clients with 128/4096-byte payloads and a
100-client, 4096-byte, 10% slow-reader case with a 50ms read delay. By default
the workload uses 1000/200/50/20 requests per client for 1/10/100/500 clients
respectively and repeats every cell three times. The script produces 108 raw
JSON files plus `summary.json`, which reports min/median/max for QPS and
latency percentiles. `BENCHMARK_REQUESTS_PER_CLIENT` can override the sample
plan for development runs.

Every raw result contains `gateway_stats.before`, `gateway_stats.after` and
counter deltas for queue rejection, slow-client close and stale-response
events. Queue peaks are process-lifetime values and are not presented as
per-cell values unless the profile has been restarted.

Run the reproducible Compose matrix with:

    CONTROL_PLANE_ADMIN_TOKEN=... GATEWAY_SHARED_TOKEN=... TOKEN_PEPPER=... scripts/benchmark_matrix.sh

When the default host ports are occupied, use a Compose override together
with `BENCHMARK_CONTROL_PLANE_URL`, `BENCHMARK_GATEWAY_HOST` and
`BENCHMARK_GATEWAY_PORT`; the service-internal ports remain unchanged.

The script requires a clean Git tree before creating the output directory or
capturing the environment. `BENCHMARK_ALLOW_DIRTY=1` is available only for
development runs and marks the captured evidence as non-final.

Final evidence must include the frozen commit SHA, `git_status=clean`, the
release-container build, the fixed matrix above, repeated raw JSON, summary
statistics and the captured environment. Benchmark output is evidence for
queue, eventfd, framing, AUTH isolation, partial-write and overload behavior.
It does not claim production capacity or availability. In the current local
container setup, process CPU/RSS samples may be unavailable across the PID
namespace boundary and are not a final headline metric when null.

Recommended wording:

> On a frozen local Compose benchmark, the gateway records latency,
> throughput, queue pressure, slow-reader behavior and request accounting
> across a controlled worker/request-queue matrix. Results are development
> evidence rather than production capacity claims.
