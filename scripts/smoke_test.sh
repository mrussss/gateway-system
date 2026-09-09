#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(docker compose)
SMOKE_ADMIN_TOKEN="${CONTROL_PLANE_ADMIN_TOKEN:-local-admin-change-me}"
SMOKE_GATEWAY_TOKEN="${GATEWAY_SHARED_TOKEN:-local-gateway-change-me}"
SMOKE_CONTROL_PLANE_URL="${SMOKE_CONTROL_PLANE_URL:-http://localhost:8080}"
SMOKE_GATEWAY_HOST="${SMOKE_GATEWAY_HOST:-127.0.0.1}"
SMOKE_GATEWAY_PORT="${SMOKE_GATEWAY_PORT:-9000}"
export CONTROL_PLANE_ADMIN_TOKEN="$SMOKE_ADMIN_TOKEN"
export GATEWAY_SHARED_TOKEN="$SMOKE_GATEWAY_TOKEN"
export TOKEN_PEPPER="${TOKEN_PEPPER:-local-pepper-change-me}"

cd "$ROOT_DIR"
"${COMPOSE[@]}" up -d --build

cleanup() {
  local status=$?
  "${COMPOSE[@]}" ps || true
  if [[ "${SMOKE_KEEP_STACK:-0}" != "1" ]]; then
    "${COMPOSE[@]}" down --volumes --remove-orphans || true
  fi
  exit "$status"
}
trap cleanup EXIT

wait_for_health() {
  local deadline=$((SECONDS + 60))
  until curl -fsS "$1" >/dev/null; do
    (( SECONDS < deadline )) || { echo "[smoke] timed out waiting for $1" >&2; return 1; }
    sleep 1
  done
}

echo "[smoke] Waiting for control plane readiness and gateway startup..."
wait_for_health "$SMOKE_CONTROL_PLANE_URL/health/ready"
"${COMPOSE[@]}" exec -T cpp-gateway sh -c 'kill -0 1' >/dev/null

for service in go-control-plane cpp-gateway; do
  uid="$(${COMPOSE[@]} exec -T "$service" id -u | tr -d '\r')"
  [[ "$uid" != "0" ]] || { echo "[smoke] $service runs as root" >&2; exit 1; }
done
[[ "$(${COMPOSE[@]} exec -T redis redis-cli PING | tr -d '\r')" == "PONG" ]]

json_field() {
  python3 -c 'import json,sys; print(json.load(sys.stdin)[sys.argv[1]])' "$1"
}

echo "[smoke] Checking token lifecycle and AUTH fail-closed behavior..."
lifecycle_client="smoke-lifecycle-$$-$RANDOM"
created="$(curl -fsS -X POST "$SMOKE_CONTROL_PLANE_URL/tokens" \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" -H "Content-Type: application/json" \
  -d "{\"client_id\":\"$lifecycle_client\"}")"
token="$(printf '%s' "$created" | json_field token)"
generation="$(printf '%s' "$created" | json_field generation)"
token_list="$(curl -fsS -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" "$SMOKE_CONTROL_PLANE_URL/tokens")"
[[ "$token_list" != *"$token"* ]]

auth_code() {
  curl -fsS -X POST "$SMOKE_CONTROL_PLANE_URL/auth/check" \
    -H "X-Gateway-Token: $SMOKE_GATEWAY_TOKEN" -H "Content-Type: application/json" \
    -d "{\"client_id\":\"$lifecycle_client\",\"token\":\"$1\"}" | json_field code
}
[[ "$(auth_code "$token")" == "OK" ]]
rotated="$(curl -fsS -X POST "$SMOKE_CONTROL_PLANE_URL/tokens/$lifecycle_client/rotate" \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" -H "If-Match: \"$generation\"")"
new_token="$(printf '%s' "$rotated" | json_field token)"
[[ "$(auth_code "$token")" == "INVALID_CREDENTIALS" ]]
[[ "$(auth_code "$new_token")" == "OK" ]]
curl -fsS -X DELETE "$SMOKE_CONTROL_PLANE_URL/tokens/$lifecycle_client" \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" >/dev/null
[[ "$(auth_code "$new_token")" == "TOKEN_DISABLED" ]]

echo "[smoke] Checking versioned Runtime Config and C++ pull path..."
etag="$(curl -fsS -D - -o /dev/null -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" \
  "$SMOKE_CONTROL_PLANE_URL/config" | tr -d '\r' | awk 'tolower($1)=="etag:" {print $2}')"
config_payload='{"max_payload_size":1048576,"max_connections_per_client":2,"max_requests_per_client_per_second":100,"slow_client_output_limit":8388608,"log_level":"INFO"}'
status="$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$SMOKE_CONTROL_PLANE_URL/config" \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" -H "If-Match: $etag" \
  -H "Content-Type: application/json" -d "$config_payload")"
[[ "$status" == "200" ]]
conflict="$(curl -sS -o /dev/null -w '%{http_code}' -X PUT "$SMOKE_CONTROL_PLANE_URL/config" \
  -H "Authorization: Bearer $SMOKE_ADMIN_TOKEN" -H "If-Match: $etag" \
  -H "Content-Type: application/json" -d "$config_payload")"
[[ "$conflict" == "409" ]]

python3 scripts/tcp_protocol_test.py \
  --host "$SMOKE_GATEWAY_HOST" \
  --port "$SMOKE_GATEWAY_PORT" \
  --control-plane-url "$SMOKE_CONTROL_PLANE_URL"
echo "[smoke] PASS"
