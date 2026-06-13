# Benchmark

`scripts/benchmark_tcp.py` is a lightweight benchmark entry point for the current TCP gateway. It is meant for local pressure checks, not for publishing fixed performance numbers.

## What It Does

Each benchmark client does the following:

- registers its own token through `POST /tokens`
- opens one TCP connection to the gateway
- sends `AUTH`
- sends the configured number of requests and waits for each response
- closes the connection

The script does not update `/config`, so it avoids mutating shared runtime limits during a benchmark run.

## Supported Arguments

```bash
python3 scripts/benchmark_tcp.py \
  --host 127.0.0.1 \
  --port 9000 \
  --control-plane http://127.0.0.1:8080 \
  --clients 5 \
  --requests-per-client 10 \
  --message echo \
  --payload "benchmark payload" \
  --client-id-prefix bench-client
```

Supported `--message` values:

- `ping`
- `echo`
- `log_push`
- `stats`

Each client uses a distinct `client_id` derived from `--client-id-prefix`.

## Output Fields

The script always prints:

- `total_requests`
- `success`
- `failed`
- `elapsed_seconds`
- `requests_per_second`
- `avg_latency_ms`
- `p95_latency_ms`

Example output shape:

```text
total_requests=50
success=50
failed=0
elapsed_seconds=0.428
requests_per_second=116.75
avg_latency_ms=7.91
p95_latency_ms=12.34
```

These numbers are examples of the output format only. Re-run the benchmark in your own environment to get real results.

## Test Environment

The current reference runs were collected on:

- `Ubuntu 22.04.5 LTS on WSL2`
- `Intel(R) Core(TM) i9-14900HX`
- `32 vCPU`
- `15 GiB RAM`
- `Docker Engine 29.5.3`
- `Docker Compose 5.1.4`
- `Test date: 2026-06-13`

These numbers are local validation results only. They are useful for this repo's current baseline, not as a production performance claim.

## Observed Results

Small local check:

```bash
python3 scripts/benchmark_tcp.py --clients 5 --requests-per-client 10
```

Observed output:

```text
total_requests=50
success=50
failed=0
elapsed_seconds=0.114
requests_per_second=439.60
avg_latency_ms=2.45
p95_latency_ms=1.02
```

Higher-pressure local check:

```bash
python3 scripts/benchmark_tcp.py --clients 50 --requests-per-client 100 --message ping
```

Observed output:

```text
total_requests=5000
success=5000
failed=0
elapsed_seconds=0.930
requests_per_second=5377.02
avg_latency_ms=6.47
p95_latency_ms=11.71
```

## Suggested Runs

Small local check:

```bash
python3 scripts/benchmark_tcp.py --clients 5 --requests-per-client 10
```

Higher-pressure local check:

```bash
python3 scripts/benchmark_tcp.py --clients 50 --requests-per-client 100 --message ping
```

Run Docker Compose first if the gateway and control plane are not already running.
