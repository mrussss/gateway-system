#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMPOSE=(docker compose)
export CONTROL_PLANE_ADMIN_TOKEN="${CONTROL_PLANE_ADMIN_TOKEN:-local-admin-change-me}"
export GATEWAY_SHARED_TOKEN="${GATEWAY_SHARED_TOKEN:-local-gateway-change-me}"
export TOKEN_PEPPER="${TOKEN_PEPPER:-local-pepper-change-me}"

cd "$ROOT_DIR"
"${COMPOSE[@]}" up -d --build

cleanup() {
  local status=$?
  "${COMPOSE[@]}" unpause redis >/dev/null 2>&1 || true
  if (( status != 0 )); then
    "${COMPOSE[@]}" ps || true
    "${COMPOSE[@]}" logs --tail=120 || true
  fi
  if [[ "${RECOVERY_KEEP_STACK:-0}" != "1" ]]; then
    "${COMPOSE[@]}" down --volumes --remove-orphans || true
  fi
  exit "$status"
}
trap cleanup EXIT

python3 scripts/redis_recovery_test.py
