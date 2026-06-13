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

- `Linux 6.6.87.2-microsoft-standard-WSL2`
- `Intel(R) Core(TM) i9-14900HX`
- `32 vCPU`
- `15 GiB RAM`

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
elapsed_seconds=0.217
requests_per_second=230.36
avg_latency_ms=4.56
p95_latency_ms=0.80
```

Higher-pressure local check:

```bash
python3 scripts/benchmark_tcp.py --clients 50 --requests-per-client 100
```

Observed output:

```text
total_requests=5000
success=5000
failed=0
elapsed_seconds=0.843
requests_per_second=5933.40
avg_latency_ms=6.46
p95_latency_ms=11.76
```

## Suggested Runs

Small local check:

```bash
python3 scripts/benchmark_tcp.py --clients 5 --requests-per-client 10
```

Higher-pressure local check:

```bash
python3 scripts/benchmark_tcp.py --clients 50 --requests-per-client 100
```

Run Docker Compose first if the gateway and control plane are not already running.
