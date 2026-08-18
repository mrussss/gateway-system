#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="fast"

usage() {
  cat <<'EOF'
Usage: scripts/release_gate.sh [--fast|--full]

  --fast  Build and run CTest, ASan/UBSan, Go Unit/Race/Vet, module,
          Compose/static Kubernetes, script syntax, and documentation checks.
  --full  Run --fast plus real Redis integration, Docker smoke/recovery,
          Kubernetes deploy/smoke/rolling update, and the Docker benchmark matrix.

Full mode requires Docker, kubectl, an existing reachable cluster, and
CONTROL_PLANE_ADMIN_TOKEN, GATEWAY_SHARED_TOKEN, and TOKEN_PEPPER. It never
creates a release tag; review and commit its raw results before tagging.
EOF
}

case "${1:---fast}" in
  --fast) mode="fast" ;;
  --full) mode="full" ;;
  -h|--help) usage; exit 0 ;;
  *) usage >&2; exit 2 ;;
esac
[[ $# -le 1 ]] || { usage >&2; exit 2; }

require_command() {
  command -v "$1" >/dev/null 2>&1 || { echo "[release-gate] missing command: $1" >&2; exit 1; }
}

run_cpp() {
  cmake -S cpp-gateway -B cpp-gateway/build \
    -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_WARNINGS_AS_ERRORS=ON
  cmake --build cpp-gateway/build --parallel
  ctest --test-dir cpp-gateway/build --output-on-failure

  cmake -S cpp-gateway -B cpp-gateway/build-sanitized \
    -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_WARNINGS_AS_ERRORS=ON \
    -DGATEWAY_ENABLE_SANITIZERS=ON
  cmake --build cpp-gateway/build-sanitized --parallel
  ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    ctest --test-dir cpp-gateway/build-sanitized --output-on-failure
}

run_go() {
  (
    cd go-control-plane
    go mod verify
    go test ./...
    go test -race ./...
    go vet ./...
  )
}

run_static() {
  docker compose config >/dev/null
  bash -n scripts/*.sh
  python3 -m compileall -q scripts cpp-gateway/scripts cpp-gateway/tests
  python3 scripts/k8s_manifest_test.py
  python3 scripts/docs_link_check.py
}

run_redis_integration() {
  docker compose up -d redis
  local deadline=$((SECONDS + 60))
  until docker compose exec -T redis redis-cli ping | grep -q PONG; do
    (( SECONDS < deadline )) || { echo "[release-gate] Redis did not become ready" >&2; return 1; }
    sleep 1
  done
  (cd go-control-plane && REDIS_TEST_ADDR=127.0.0.1:6379 go test -count=1 -run '^TestRedis' ./internal/app)
  docker compose down --volumes --remove-orphans
}

cleanup() {
  local status=$?
  if [[ "$mode" == "full" && $status -ne 0 ]]; then
    docker compose ps || true
    docker compose logs --tail=100 || true
  fi
  if [[ "$mode" == "full" ]]; then
    docker compose down --volumes --remove-orphans || true
  fi
  exit "$status"
}
trap cleanup EXIT

cd "$ROOT_DIR"
for command in cmake ctest go docker python3; do
  require_command "$command"
done

echo "[release-gate] mode=$mode head=$(git rev-parse HEAD)"
run_cpp
run_go
run_static

if [[ "$mode" == "full" ]]; then
  for command in curl kubectl; do
    require_command "$command"
  done
  : "${CONTROL_PLANE_ADMIN_TOKEN:?CONTROL_PLANE_ADMIN_TOKEN must be set for --full}"
  : "${GATEWAY_SHARED_TOKEN:?GATEWAY_SHARED_TOKEN must be set for --full}"
  : "${TOKEN_PEPPER:?TOKEN_PEPPER must be set for --full}"
  run_redis_integration
  bash scripts/smoke_test.sh
  bash scripts/redis_recovery_test.sh
  bash scripts/k8s_deploy.sh
  bash scripts/k8s_smoke.sh
  bash scripts/k8s_rolling_update_test.sh
  scripts/benchmark_matrix.sh
fi

echo "[release-gate] PASS mode=$mode"
