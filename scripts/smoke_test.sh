#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(docker compose)
SMOKE_ADMIN_TOKEN="${CONTROL_PLANE_ADMIN_TOKEN:-local-admin-change-me}"
SMOKE_GATEWAY_TOKEN="${GATEWAY_SHARED_TOKEN:-local-gateway-change-me}"
export CONTROL_PLANE_ADMIN_TOKEN="$SMOKE_ADMIN_TOKEN"
export GATEWAY_SHARED_TOKEN="$SMOKE_GATEWAY_TOKEN"
export TOKEN_PEPPER="${TOKEN_PEPPER:-local-pepper-change-me}"

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
  if [[ "${SMOKE_KEEP_STACK:-0}" != "1" ]]; then
    "${COMPOSE[@]}" down --volumes --remove-orphans || true
  fi
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

expect_admin_ok() {
  local name="$1"
  local url="$2"
  echo "[smoke] Checking $name: $url"
  curl -fsS -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" "$url"
  echo
}

echo "[smoke] Waiting for Go control plane liveness and readiness..."
wait_for_health "http://localhost:8080/health/live"
wait_for_health "http://localhost:8080/health/ready"

expect_http_ok "liveness" "http://localhost:8080/health/live"
expect_http_ok "readiness" "http://localhost:8080/health/ready"

echo "[smoke] Checking non-root, read-only application containers..."
for service in go-control-plane cpp-gateway; do
  uid="$("${COMPOSE[@]}" exec -T "$service" id -u | tr -d '\r')"
  [[ "$uid" != "0" ]] || { echo "[smoke] FAIL: $service runs as root" >&2; exit 1; }
  if "${COMPOSE[@]}" exec -T "$service" sh -c 'touch /rootfs-write-probe' >/dev/null 2>&1; then
    echo "[smoke] FAIL: $service root filesystem is writable" >&2
    exit 1
  fi
done

echo "[smoke] Checking Redis connectivity..."
redis_ping="$("${COMPOSE[@]}" exec -T redis redis-cli PING)"
if [[ "$redis_ping" != "PONG" ]]; then
  echo "[smoke] FAIL: unexpected Redis PING response: $redis_ping" >&2
  exit 1
fi
echo "$redis_ping"

echo "[smoke] Waiting for gateway metrics report..."
deadline=$((SECONDS + 70))
until gateway_status="$(curl -fsS -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" "http://localhost:8080/gateway/status")"; do
  if (( SECONDS >= deadline )); then
    echo "[smoke] FAIL: timed out waiting for /gateway/status" >&2
    exit 1
  fi
  sleep 1
done
echo "$gateway_status"

expect_admin_ok "gateways" "http://localhost:8080/gateways"
expect_admin_ok "gateway status by id" "http://localhost:8080/gateways/gateway-001/status"

for path in "/gateway/status" "/gateways/gateway-001/status"; do
  echo "[smoke] Checking liveness fields on $path"
  body="$(curl -fsS -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" "http://localhost:8080$path")"
  [[ "$body" == *"\"online\""* ]] || { echo "[smoke] FAIL: missing online in $path" >&2; exit 1; }
  [[ "$body" == *"\"status\""* ]] || { echo "[smoke] FAIL: missing status in $path" >&2; exit 1; }
  [[ "$body" == *"\"seconds_since_last_report\""* ]] || { echo "[smoke] FAIL: missing seconds_since_last_report in $path" >&2; exit 1; }
done

expect_admin_ok "clients" "http://localhost:8080/clients"

echo "[smoke] Waiting for gateway clients by id..."
deadline=$((SECONDS + 70))
until gateway_clients="$(curl -fsS -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" "http://localhost:8080/gateways/gateway-001/clients")"; do
  if (( SECONDS >= deadline )); then
    echo "[smoke] FAIL: timed out waiting for /gateways/gateway-001/clients" >&2
    exit 1
  fi
  sleep 1
done
echo "$gateway_clients"

echo "[smoke] Checking Prometheus exposition..."
prometheus_body="$(curl -fsS http://localhost:8080/metrics)"
for metric in \
  control_plane_http_requests_total \
  control_plane_http_request_duration_seconds \
  gateway_active_connections \
  gateway_online; do
  [[ "$prometheus_body" == *"$metric"* ]] || {
    echo "[smoke] FAIL: /metrics missing $metric" >&2
    exit 1
  }
done

echo "[smoke] Checking Redis snapshot TTLs..."
for key in gateway:status:gateway-001 gateway:clients:gateway-001; do
  ttl="$("${COMPOSE[@]}" exec -T redis redis-cli TTL "$key" | tr -d '\r')"
  if ! [[ "$ttl" =~ ^[0-9]+$ ]] || (( ttl <= 0 )); then
    echo "[smoke] FAIL: key $key has invalid TTL $ttl" >&2
    exit 1
  fi
done

json_field() {
  local field="$1"
  python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$field"
}

assert_auth() {
  local client_id="$1"
  local token="$2"
  local expected_code="$3"
  local response
  local code
  response="$(curl -fsS -X POST http://localhost:8080/auth/check \
    -H "X-Gateway-Token: $SMOKE_GATEWAY_TOKEN" \
    -H "Content-Type: application/json" \
    -d "{\"client_id\":\"$client_id\",\"token\":\"$token\"}")"
  code="$(printf '%s' "$response" | json_field code)"
  if [[ "$code" != "$expected_code" ]]; then
    echo "[smoke] FAIL: auth code=$code, expected=$expected_code" >&2
    exit 1
  fi
}

echo "[smoke] Checking one-time token create, rotate, and disable..."
lifecycle_client="smoke-lifecycle-$$-$RANDOM"
created="$(curl -fsS -X POST http://localhost:8080/tokens \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  -d "{\"client_id\":\"$lifecycle_client\"}")"
old_token="$(printf '%s' "$created" | json_field token)"
generation="$(printf '%s' "$created" | json_field generation)"
token_list="$(curl -fsS -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" http://localhost:8080/tokens)"
[[ "$token_list" == *"$lifecycle_client"* ]] || { echo "[smoke] FAIL: token metadata missing" >&2; exit 1; }
[[ "$token_list" != *"$old_token"* ]] || { echo "[smoke] FAIL: token list exposed plaintext" >&2; exit 1; }
assert_auth "$lifecycle_client" "$old_token" "OK"

rotated="$(curl -fsS -X POST "http://localhost:8080/tokens/$lifecycle_client/rotate" \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" \
  -H "If-Match: \"$generation\"")"
new_token="$(printf '%s' "$rotated" | json_field token)"
[[ "$new_token" != "$old_token" ]] || { echo "[smoke] FAIL: rotation reused token" >&2; exit 1; }
assert_auth "$lifecycle_client" "$old_token" "INVALID_CREDENTIALS"
assert_auth "$lifecycle_client" "$new_token" "OK"

curl -fsS -X DELETE "http://localhost:8080/tokens/$lifecycle_client" \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" >/dev/null
assert_auth "$lifecycle_client" "$new_token" "TOKEN_DISABLED"

echo "[smoke] Checking config ETag compare-and-set..."
etag="$(curl -fsS -D - -o /dev/null \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" \
  http://localhost:8080/config | tr -d '\r' | awk 'tolower($1)=="etag:" {print $2}')"
[[ -n "$etag" ]] || { echo "[smoke] FAIL: config ETag missing" >&2; exit 1; }
config_payload='{"max_payload_size":1048576,"max_connections_per_client":2,"max_requests_per_client_per_second":100,"slow_client_output_limit":8388608,"log_level":"INFO"}'
update_status="$(curl -sS -o /dev/null -w '%{http_code}' -X PUT http://localhost:8080/config \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" -H "If-Match: $etag" \
  -H "Content-Type: application/json" -d "$config_payload")"
[[ "$update_status" == "200" ]] || { echo "[smoke] FAIL: config update status $update_status" >&2; exit 1; }
conflict_status="$(curl -sS -o /dev/null -w '%{http_code}' -X PUT http://localhost:8080/config \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" -H "If-Match: $etag" \
  -H "Content-Type: application/json" -d "$config_payload")"
[[ "$conflict_status" == "409" ]] || { echo "[smoke] FAIL: stale config status $conflict_status" >&2; exit 1; }
prometheus_body="$(curl -fsS http://localhost:8080/metrics)"
for metric in control_plane_auth_total control_plane_config_updates_total control_plane_config_conflicts_total; do
  [[ "$prometheus_body" == *"$metric"* ]] || {
    echo "[smoke] FAIL: /metrics missing exercised metric $metric" >&2
    exit 1
  }
done

echo "[smoke] Running TCP protocol checks against localhost:9000..."
python3 scripts/tcp_protocol_test.py

echo "[smoke] PASS"
