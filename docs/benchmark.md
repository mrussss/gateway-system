# Benchmark

The benchmark exercises authenticated TCP throughput and latency across a
controlled worker/request-queue matrix, payload sizes, concurrent clients and
slow readers. It records request success/failure accounting, latency
percentiles, AUTH latency and optional Linux process CPU/RSS samples.

The final matrix keeps the response queue and AUTH controls fixed:

| Profile | Workers | Request Queue | Response Queue | AUTH Workers | AUTH Queue |
| --- | ---: | ---: | ---: | ---: | ---: |
| `workers1-q64` | 1 | 64 | 4096 | 2 | 32 |
| `workers1-q4096` | 1 | 4096 | 4096 | 2 | 32 |
| `workers4-q64` | 4 | 64 | 4096 | 2 | 32 |
| `workers4-q4096` | 4 | 4096 | 4096 | 2 | 32 |

Each profile covers 1/10/100/500 clients with 128/4096-byte payloads and a
100-client, 4096-byte, 10% slow-reader case with a 50ms read delay. The script
produces 36 raw JSON files and validates request accounting before passing.

Run the reproducible Compose matrix with:

    CONTROL_PLANE_ADMIN_TOKEN=... GATEWAY_SHARED_TOKEN=... TOKEN_PEPPER=... scripts/benchmark_matrix.sh

The script requires a clean Git tree before creating the output directory or
capturing the environment. `BENCHMARK_ALLOW_DIRTY=1` is available only for
development runs and marks the captured evidence as non-final.

Final evidence must include the frozen commit SHA, `git_status=clean`, the
release-container build, the fixed matrix above, raw JSON and the captured
environment. Benchmark output is evidence for queue, eventfd, framing,
AUTH isolation, partial-write and overload behavior. It does not scrape a
monitoring endpoint and does not claim production capacity or availability.

Recommended wording:

> On a frozen local Compose benchmark, the gateway records latency,
> throughput, queue pressure, slow-reader behavior and request accounting
> across a controlled worker/request-queue matrix. Results are development
> evidence rather than production capacity claims.
