#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(docker compose)

cd "$ROOT_DIR"

echo "[smoke] Starting services with Docker Compose..."
"${COMPOSE[@]}" up -d --build

cleanup() {
  local status=$?
  echo
  echo "[smoke] docker compose ps"
  "${COMPOSE[@]}" ps || true
  echo
  echo "[smoke] Recent service logs"
  "${COMPOSE[@]}" logs --tail=80 || true
  exit "$status"
}
trap cleanup EXIT

wait_for_health() {
  local url="$1"
  local deadline=$((SECONDS + 60))

  until curl -fsS "$url" >/dev/null; do
    if (( SECONDS >= deadline )); then
      echo "[smoke] FAIL: timed out waiting for $url" >&2
      return 1
    fi
    sleep 1
  done
}

expect_http_ok() {
  local name="$1"
  local url="$2"
  echo "[smoke] Checking $name: $url"
  curl -fsS "$url"
  echo
}

echo "[smoke] Waiting for Go control plane health..."
wait_for_health "http://localhost:8080/health"

expect_http_ok "health" "http://localhost:8080/health"

echo "[smoke] Checking Redis connectivity..."
redis_ping="$("${COMPOSE[@]}" exec -T redis redis-cli PING)"
if [[ "$redis_ping" != "PONG" ]]; then
  echo "[smoke] FAIL: unexpected Redis PING response: $redis_ping" >&2
  exit 1
fi
echo "$redis_ping"

echo "[smoke] Waiting for gateway metrics report..."
deadline=$((SECONDS + 70))
until curl -fsS "http://localhost:8080/gateway/status" >/tmp/gateway_status.json; do
  if (( SECONDS >= deadline )); then
    echo "[smoke] FAIL: timed out waiting for /gateway/status" >&2
    exit 1
  fi
  sleep 1
done
cat /tmp/gateway_status.json
echo

expect_http_ok "gateways" "http://localhost:8080/gateways"
expect_http_ok "gateway status by id" "http://localhost:8080/gateways/gateway-001/status"

for path in "/gateway/status" "/gateways/gateway-001/status"; do
  echo "[smoke] Checking liveness fields on $path"
  body="$(curl -fsS "http://localhost:8080$path")"
  [[ "$body" == *"\"online\""* ]] || { echo "[smoke] FAIL: missing online in $path" >&2; exit 1; }
  [[ "$body" == *"\"status\""* ]] || { echo "[smoke] FAIL: missing status in $path" >&2; exit 1; }
  [[ "$body" == *"\"seconds_since_last_report\""* ]] || { echo "[smoke] FAIL: missing seconds_since_last_report in $path" >&2; exit 1; }
done

expect_http_ok "clients" "http://localhost:8080/clients"

echo "[smoke] Waiting for gateway clients by id..."
deadline=$((SECONDS + 70))
until curl -fsS "http://localhost:8080/gateways/gateway-001/clients" >/tmp/gateway_clients.json; do
  if (( SECONDS >= deadline )); then
    echo "[smoke] FAIL: timed out waiting for /gateways/gateway-001/clients" >&2
    exit 1
  fi
  sleep 1
done
cat /tmp/gateway_clients.json
echo

echo "[smoke] Running TCP protocol checks against localhost:9000..."
python3 scripts/tcp_protocol_test.py

echo "[smoke] PASS"
