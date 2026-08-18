#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(docker compose)
run_id="${BENCHMARK_RUN_ID:-$(date -u +%Y%m%dT%H%M%SZ)}"
output_dir="${BENCHMARK_OUTPUT_DIR:-$ROOT_DIR/results/benchmark/$run_id}"
requests_per_client="${BENCHMARK_REQUESTS_PER_CLIENT:-20}"
export CONTROL_PLANE_ADMIN_TOKEN="${CONTROL_PLANE_ADMIN_TOKEN:-local-admin-change-me}"
export GATEWAY_SHARED_TOKEN="${GATEWAY_SHARED_TOKEN:-local-gateway-change-me}"
export TOKEN_PEPPER="${TOKEN_PEPPER:-local-pepper-change-me}"

mkdir -p "$output_dir"
cd "$ROOT_DIR"
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
  until curl -fsS http://127.0.0.1:8080/health/ready >/dev/null; do
    (( SECONDS < deadline )) || { echo "[benchmark] control plane not ready" >&2; return 1; }
    sleep 1
  done
  until "${COMPOSE[@]}" exec -T cpp-gateway sh -c 'kill -0 1' >/dev/null 2>&1; do
    (( SECONDS < deadline )) || { echo "[benchmark] gateway not ready" >&2; return 1; }
    sleep 1
  done
  sleep 6
}

"${COMPOSE[@]}" build
for profile in "workers1-q64:1:64:64" "workers4-q4096:4:4096:4096"; do
  IFS=: read -r profile_name worker_count request_capacity response_capacity <<<"$profile"
  export WORKER_COUNT="$worker_count"
  export REQUEST_QUEUE_CAPACITY="$request_capacity"
  export RESPONSE_QUEUE_CAPACITY="$response_capacity"
  "${COMPOSE[@]}" up -d --force-recreate
  wait_ready

  gateway_pid="$(docker inspect -f '{{.State.Pid}}' "$("${COMPOSE[@]}" ps -q cpp-gateway)")"
  for clients in 1 10 100 500; do
    for payload_size in 128 4096; do
      python3 scripts/benchmark_tcp.py \
        --mode steady \
        --build-mode Release-container \
        --clients "$clients" \
        --requests-per-client "$requests_per_client" \
        --payload-size "$payload_size" \
        --gateway-pid "$gateway_pid" \
        --worker-count "$worker_count" \
        --request-queue-capacity "$request_capacity" \
        --response-queue-capacity "$response_capacity" \
        --run-id "$run_id-$profile_name-$clients-$payload_size" \
        --output "$output_dir/$profile_name-clients$clients-payload$payload_size.json"
    done
  done

  python3 scripts/benchmark_tcp.py \
    --mode steady \
    --build-mode Release-container \
    --clients 100 \
    --requests-per-client "$requests_per_client" \
    --payload-size 4096 \
    --slow-client-ratio 0.10 \
    --slow-read-delay-ms 50 \
    --gateway-pid "$gateway_pid" \
    --worker-count "$worker_count" \
    --request-queue-capacity "$request_capacity" \
    --response-queue-capacity "$response_capacity" \
    --run-id "$run_id-$profile_name-slow" \
    --output "$output_dir/$profile_name-slow10pct.json"
done

echo "[benchmark] PASS results=$output_dir"
