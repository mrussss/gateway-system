#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(docker compose)
run_id="${BENCHMARK_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
output_dir="${BENCHMARK_OUTPUT_DIR:-$ROOT_DIR/results/benchmark/$run_id}"
repeats="${BENCHMARK_REPEATS:-3}"
control_plane_url="${BENCHMARK_CONTROL_PLANE_URL:-http://127.0.0.1:8080}"
gateway_host="${BENCHMARK_GATEWAY_HOST:-127.0.0.1}"
gateway_port="${BENCHMARK_GATEWAY_PORT:-9000}"
export CONTROL_PLANE_ADMIN_TOKEN="${CONTROL_PLANE_ADMIN_TOKEN:-local-admin-change-me}"
export GATEWAY_SHARED_TOKEN="${GATEWAY_SHARED_TOKEN:-local-gateway-change-me}"
export TOKEN_PEPPER="${TOKEN_PEPPER:-local-pepper-change-me}"

cd "$ROOT_DIR"
if ! [[ "$repeats" =~ ^[1-9][0-9]*$ ]]; then
  echo "[benchmark] BENCHMARK_REPEATS must be a positive integer" >&2
  exit 1
fi
if [[ "${BENCHMARK_ALLOW_DIRTY:-0}" != "1" ]]; then
  if ! git diff --quiet || ! git diff --cached --quiet ||
     [[ -n "$(git status --porcelain --untracked-files=all)" ]]; then
    echo "[benchmark] refusing to run with a dirty working tree" >&2
    echo "[benchmark] set BENCHMARK_ALLOW_DIRTY=1 only for development runs" >&2
    exit 1
  fi
else
  echo "[benchmark] WARNING: BENCHMARK_ALLOW_DIRTY=1; evidence is not final" >&2
fi

mkdir -p "$output_dir"
scripts/capture_environment.sh "$output_dir/environment.txt" >/dev/null

cleanup() {
  local status=$?
  if (( status != 0 )); then
    "${COMPOSE[@]}" ps || true
    "${COMPOSE[@]}" logs --tail=100 || true
  fi
  if [[ "${BENCHMARK_KEEP_STACK:-0}" != "1" ]]; then
    "${COMPOSE[@]}" down --volumes --remove-orphans || true
  fi
  exit "$status"
}
trap cleanup EXIT

wait_ready() {
  local deadline=$((SECONDS + 90))
  until curl -fsS "$control_plane_url/health/ready" >/dev/null; do
    (( SECONDS < deadline )) || { echo "[benchmark] control plane not ready" >&2; return 1; }
    sleep 1
  done
  until "${COMPOSE[@]}" exec -T cpp-gateway sh -c 'kill -0 1' >/dev/null 2>&1; do
    (( SECONDS < deadline )) || { echo "[benchmark] gateway not ready" >&2; return 1; }
    sleep 1
  done
  sleep 6
}

export AUTH_WORKER_COUNT=2
export AUTH_QUEUE_CAPACITY=32

requests_for_clients() {
  if [[ -n "${BENCHMARK_REQUESTS_PER_CLIENT:-}" ]]; then
    echo "$BENCHMARK_REQUESTS_PER_CLIENT"
    return
  fi
  case "$1" in
    1) echo "${BENCHMARK_REQUESTS_1:-1000}" ;;
    10) echo "${BENCHMARK_REQUESTS_10:-200}" ;;
    100) echo "${BENCHMARK_REQUESTS_100:-50}" ;;
    500) echo "${BENCHMARK_REQUESTS_500:-20}" ;;
    *) echo "[benchmark] unsupported client count: $1" >&2; return 1 ;;
  esac
}

cat >>"$output_dir/environment.txt" <<EOF

[benchmark]
run_id=$run_id
repeats=$repeats
requests_1=$(requests_for_clients 1)
requests_10=$(requests_for_clients 10)
requests_100=$(requests_for_clients 100)
requests_500=$(requests_for_clients 500)
response_queue_capacity=4096
auth_worker_count=2
auth_queue_capacity=32
gateway_host=$gateway_host
gateway_port=$gateway_port
control_plane_url=$control_plane_url
EOF

"${COMPOSE[@]}" build
for profile in \
  "workers1-q64:1:64:4096" \
  "workers1-q4096:1:4096:4096" \
  "workers4-q64:4:64:4096" \
  "workers4-q4096:4:4096:4096"; do
  IFS=: read -r profile_name worker_count request_capacity response_capacity <<<"$profile"
  export WORKER_COUNT="$worker_count"
  export REQUEST_QUEUE_CAPACITY="$request_capacity"
  export RESPONSE_QUEUE_CAPACITY="$response_capacity"
  "${COMPOSE[@]}" up -d --force-recreate
  wait_ready

  gateway_pid="$(docker inspect -f '{{.State.Pid}}' "$("${COMPOSE[@]}" ps -q cpp-gateway)")"
  for ((repeat_index = 1; repeat_index <= repeats; repeat_index++)); do
    for clients in 1 10 100 500; do
      requests_per_client="$(requests_for_clients "$clients")"
      for payload_size in 128 4096; do
        python3 scripts/benchmark_tcp.py \
          --mode steady \
          --build-mode Release-container \
          --profile-name "$profile_name" \
          --repeat-index "$repeat_index" \
          --capture-gateway-stats \
          --host "$gateway_host" \
          --port "$gateway_port" \
          --control-plane "$control_plane_url" \
          --clients "$clients" \
          --requests-per-client "$requests_per_client" \
          --payload-size "$payload_size" \
          --gateway-pid "$gateway_pid" \
          --worker-count "$worker_count" \
          --request-queue-capacity "$request_capacity" \
          --response-queue-capacity "$response_capacity" \
          --allow-request-failures \
          --run-id "$run_id-$profile_name-r$repeat_index-$clients-$payload_size" \
          --output "$output_dir/$profile_name-repeat$repeat_index-clients$clients-payload$payload_size.json"
      done
    done

    slow_requests_per_client="$(requests_for_clients 100)"
    python3 scripts/benchmark_tcp.py \
      --mode steady \
      --build-mode Release-container \
      --profile-name "$profile_name" \
      --repeat-index "$repeat_index" \
      --capture-gateway-stats \
      --host "$gateway_host" \
      --port "$gateway_port" \
      --control-plane "$control_plane_url" \
      --clients 100 \
      --requests-per-client "$slow_requests_per_client" \
      --payload-size 4096 \
      --slow-client-ratio 0.10 \
      --slow-read-delay-ms 50 \
      --gateway-pid "$gateway_pid" \
      --worker-count "$worker_count" \
      --request-queue-capacity "$request_capacity" \
      --response-queue-capacity "$response_capacity" \
      --allow-request-failures \
      --run-id "$run_id-$profile_name-r$repeat_index-slow" \
      --output "$output_dir/$profile_name-repeat$repeat_index-slow10pct.json"
  done
done

python3 - "$output_dir" "$repeats" <<'PY'
import glob
import json
import pathlib
import statistics
import sys

directory = pathlib.Path(sys.argv[1])
repeats = int(sys.argv[2])
files = sorted(glob.glob(str(directory / "*.json")))
expected = repeats * 36
if len(files) != expected:
    raise SystemExit(f"expected {expected} benchmark JSON files, found {len(files)}")
groups = {}
for path in files:
    result = json.load(open(path, encoding="utf-8"))
    requests = result["requests"]
    if requests["attempted"] != requests["success"] + requests["failed"]:
        raise SystemExit(f"request accounting mismatch: {path}")
    if requests["success"] == 0:
        raise SystemExit(f"no successful requests: {path}")
    stats = result.get("gateway_stats")
    if not isinstance(stats, dict) or not isinstance(stats.get("before"), dict) or not isinstance(stats.get("after"), dict):
        raise SystemExit(f"missing gateway STATS before/after: {path}")
    parameters = result["parameters"]
    key = (
        parameters["profile_name"],
        parameters["clients"],
        parameters["payload_bytes"],
        parameters["slow_client_ratio"],
    )
    groups.setdefault(key, []).append(result)

summary = {"schema_version": 1, "repeats": repeats, "cells": []}
for key, values in sorted(groups.items()):
    profile, clients, payload_bytes, slow_ratio = key
    cell = {
        "profile": profile,
        "clients": clients,
        "payload_bytes": payload_bytes,
        "slow_client_ratio": slow_ratio,
        "runs": [value["parameters"]["repeat_index"] for value in values],
        "metrics": {},
    }
    for metric, path in (
        ("qps", ("requests", "qps")),
        ("p50_ms", ("requests", "latency_ms", "p50")),
        ("p95_ms", ("requests", "latency_ms", "p95")),
        ("p99_ms", ("requests", "latency_ms", "p99")),
    ):
        samples = []
        for value in values:
            current = value
            for part in path:
                current = current[part]
            samples.append(float(current))
        cell["metrics"][metric] = {
            "min": min(samples),
            "median": statistics.median(samples),
            "max": max(samples),
        }
    summary["cells"].append(cell)
(directory / "summary.json").write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"[benchmark] validated {len(files)} raw result files across {len(groups)} cells")
PY

echo "[benchmark] PASS results=$output_dir"
