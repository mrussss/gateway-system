#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAMESPACE="gateway-system"
CONTROL_PORT="${K8S_CONTROL_LOCAL_PORT:-18080}"
GATEWAY_PORT="${K8S_GATEWAY_LOCAL_PORT:-19000}"
export CONTROL_PLANE_ADMIN_TOKEN="${CONTROL_PLANE_ADMIN_TOKEN:?CONTROL_PLANE_ADMIN_TOKEN must be set}"
TMP_DIR="$(mktemp -d)"

cd "$ROOT_DIR"
kubectl -n "$NAMESPACE" rollout status statefulset/redis --timeout=180s
kubectl -n "$NAMESPACE" rollout status deployment/control-plane --timeout=180s
kubectl -n "$NAMESPACE" rollout status deployment/gateway --timeout=180s

ready_replicas="$(kubectl -n "$NAMESPACE" get deployment gateway -o jsonpath='{.status.readyReplicas}')"
[[ "$ready_replicas" == "2" ]] || { echo "[k8s-smoke] expected two ready gateways, got $ready_replicas" >&2; exit 1; }

kubectl -n "$NAMESPACE" port-forward service/control-plane "$CONTROL_PORT:8080" >"$TMP_DIR/control-forward.log" 2>&1 &
control_forward_pid=$!
kubectl -n "$NAMESPACE" port-forward service/gateway "$GATEWAY_PORT:9000" >"$TMP_DIR/gateway-forward.log" 2>&1 &
gateway_forward_pid=$!
cleanup() {
  local status=$?
  kill "$control_forward_pid" "$gateway_forward_pid" >/dev/null 2>&1 || true
  wait "$control_forward_pid" "$gateway_forward_pid" >/dev/null 2>&1 || true
  if (( status != 0 )); then
    kubectl -n "$NAMESPACE" get pods -o wide || true
    kubectl -n "$NAMESPACE" logs -l app.kubernetes.io/name=gateway --tail=100 --prefix=true || true
  fi
  find "$TMP_DIR" -type f -delete
  rmdir "$TMP_DIR"
  exit "$status"
}
trap cleanup EXIT

deadline=$((SECONDS + 30))
until curl -fsS "http://127.0.0.1:$CONTROL_PORT/health/ready" >/dev/null; do
  (( SECONDS < deadline )) || { echo "[k8s-smoke] control plane port-forward not ready" >&2; exit 1; }
  sleep 1
done

for deployment in control-plane gateway; do
  uid="$(kubectl -n "$NAMESPACE" exec deployment/"$deployment" -- id -u | tr -d '\r')"
  [[ "$uid" != "0" ]] || { echo "[k8s-smoke] $deployment runs as root" >&2; exit 1; }
  if kubectl -n "$NAMESPACE" exec deployment/"$deployment" -- sh -c 'touch /rootfs-write-probe' >/dev/null 2>&1; then
    echo "[k8s-smoke] $deployment root filesystem is writable" >&2
    exit 1
  fi
done

python3 scripts/tcp_protocol_test.py \
  --host 127.0.0.1 \
  --port "$GATEWAY_PORT" \
  --control-plane-url "http://127.0.0.1:$CONTROL_PORT"

metrics="$(curl -fsS "http://127.0.0.1:$CONTROL_PORT/metrics")"
for metric in gateway_active_connections gateway_online control_plane_http_requests_total; do
  [[ "$metrics" == *"$metric"* ]] || { echo "[k8s-smoke] /metrics missing $metric" >&2; exit 1; }
done

gateway_pod="$(kubectl -n "$NAMESPACE" get pod -l app.kubernetes.io/name=gateway -o jsonpath='{.items[0].metadata.name}')"
redis_pod="$(kubectl -n "$NAMESPACE" get pod -l app.kubernetes.io/name=redis -o jsonpath='{.items[0].metadata.name}')"
ttl="$(kubectl -n "$NAMESPACE" exec "$redis_pod" -- redis-cli TTL "gateway:status:$gateway_pod" | tr -d '\r')"
[[ "$ttl" =~ ^[0-9]+$ ]] && (( ttl > 0 )) || { echo "[k8s-smoke] invalid gateway status TTL: $ttl" >&2; exit 1; }

echo "[k8s-smoke] PASS two replicas, security context, protocol, Prometheus, and Redis TTL"
