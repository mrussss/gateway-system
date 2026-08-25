# Benchmark

The benchmark exercises authenticated TCP throughput and latency across normal
worker counts, bounded queue capacities, payload sizes, concurrent clients and
slow readers. It records request success/failure accounting, latency percentiles,
AUTH latency and optional Linux process CPU/RSS samples.

Run the reproducible Compose matrix with:

    CONTROL_PLANE_ADMIN_TOKEN=... GATEWAY_SHARED_TOKEN=... TOKEN_PEPPER=... scripts/benchmark_matrix.sh

Benchmark output is evidence for queue, eventfd, framing, AUTH isolation,
partial-write and overload behavior. It does not scrape a monitoring endpoint
and does not claim production capacity or availability.
