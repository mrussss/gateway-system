#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAMESPACE="gateway-system"
CONTROL_PORT="${K8S_CONTROL_LOCAL_PORT:-18080}"
GATEWAY_PORT="${K8S_GATEWAY_LOCAL_PORT:-19000}"
export CONTROL_PLANE_ADMIN_TOKEN="${CONTROL_PLANE_ADMIN_TOKEN:?CONTROL_PLANE_ADMIN_TOKEN must be set}"
TMP_DIR="$(mktemp -d)"

cd "$ROOT_DIR"
kubectl -n "$NAMESPACE" rollout status deployment/gateway --timeout=180s
old_pods="$(kubectl -n "$NAMESPACE" get pods -l app.kubernetes.io/name=gateway -o jsonpath='{range .items[*]}{.metadata.name}{" "}{end}')"
old_ips="$(kubectl -n "$NAMESPACE" get pods -l app.kubernetes.io/name=gateway -o jsonpath='{range .items[*]}{.status.podIP}{" "}{end}')"
old_count="$(wc -w <<<"$old_pods" | tr -d ' ')"
[[ "$old_count" == "2" ]] || { echo "[k8s-roll] expected two starting gateway pods, got: $old_pods" >&2; exit 1; }

kubectl -n "$NAMESPACE" port-forward service/control-plane "$CONTROL_PORT:8080" >"$TMP_DIR/control-forward.log" 2>&1 &
control_forward_pid=$!
(
  while true; do
    kubectl -n "$NAMESPACE" port-forward service/gateway "$GATEWAY_PORT:9000" >>"$TMP_DIR/gateway-forward.log" 2>&1 || true
    sleep 0.2
  done
) &
gateway_forward_pid=$!
kubectl -n "$NAMESPACE" logs -f -l app.kubernetes.io/name=gateway --all-containers=true --prefix=true >"$TMP_DIR/gateway-rollout.log" 2>&1 &
log_pid=$!

cleanup() {
  local status=$?
  kill "$control_forward_pid" "$gateway_forward_pid" "$log_pid" "${client_pid:-}" >/dev/null 2>&1 || true
  wait "$control_forward_pid" "$gateway_forward_pid" "$log_pid" "${client_pid:-}" >/dev/null 2>&1 || true
  if (( status != 0 )); then
    kubectl -n "$NAMESPACE" get pods -o wide || true
    tail -200 "$TMP_DIR/gateway-rollout.log" || true
  fi
  if [[ "${K8S_KEEP_RESULTS:-0}" == "1" ]]; then
    echo "[k8s-roll] retained results in $TMP_DIR"
  else
    find "$TMP_DIR" -type f -delete
    rmdir "$TMP_DIR"
  fi
  exit "$status"
}
trap cleanup EXIT

deadline=$((SECONDS + 30))
until curl -fsS "http://127.0.0.1:$CONTROL_PORT/health/ready" >/dev/null; do
  (( SECONDS < deadline )) || { echo "[k8s-roll] control plane port-forward not ready" >&2; exit 1; }
  sleep 1
done

python3 scripts/k8s_rolling_client.py \
  --port "$GATEWAY_PORT" \
  --control-plane-url "http://127.0.0.1:$CONTROL_PORT" \
  --duration 45 \
  --ready-file "$TMP_DIR/client.ready" \
  --result-file "$TMP_DIR/client-result.json" &
client_pid=$!

deadline=$((SECONDS + 20))
until [[ -f "$TMP_DIR/client.ready" ]]; do
  kill -0 "$client_pid" 2>/dev/null || { echo "[k8s-roll] client exited before becoming ready" >&2; exit 1; }
  (( SECONDS < deadline )) || { echo "[k8s-roll] client did not become ready" >&2; exit 1; }
  sleep 0.5
done

kubectl -n "$NAMESPACE" rollout restart deployment/gateway

deadline=$((SECONDS + 90))
while (( SECONDS < deadline )); do
  endpoint_ips="$(kubectl -n "$NAMESPACE" get endpoints gateway -o jsonpath='{.subsets[*].addresses[*].ip}')"
  old_endpoint_found=0
  for old_ip in $old_ips; do
    if [[ " $endpoint_ips " == *" $old_ip "* ]]; then
      old_endpoint_found=1
    fi
  done
  (( old_endpoint_found == 0 )) && break
  sleep 1
done
(( old_endpoint_found == 0 )) || { echo "[k8s-roll] old gateway endpoints were not removed" >&2; exit 1; }

redis_pod="$(kubectl -n "$NAMESPACE" get pod -l app.kubernetes.io/name=redis -o jsonpath='{.items[0].metadata.name}')"
for old_ip in $old_ips; do
  if kubectl -n "$NAMESPACE" exec "$redis_pod" -- nc -z -w 1 "$old_ip" 9000 >/dev/null 2>&1; then
    echo "[k8s-roll] old listener still accepted connections at $old_ip" >&2
    exit 1
  fi
done

kubectl -n "$NAMESPACE" rollout status deployment/gateway --timeout=180s
wait "$client_pid"
unset client_pid

new_pods="$(kubectl -n "$NAMESPACE" get pods -l app.kubernetes.io/name=gateway -o jsonpath='{range .items[*]}{.metadata.name}{" "}{end}')"
for old_pod in $old_pods; do
  [[ " $new_pods " != *" $old_pod "* ]] || { echo "[k8s-roll] old pod still present: $old_pod" >&2; exit 1; }
done

sleep 1
grep -q "graceful shutdown started" "$TMP_DIR/gateway-rollout.log" || {
  echo "[k8s-roll] DRAINING start was not observed in gateway logs" >&2
  exit 1
}
metrics="$(curl -fsS "http://127.0.0.1:$CONTROL_PORT/metrics")"
for new_pod in $new_pods; do
  [[ "$metrics" == *"gateway_id=\"$new_pod\""* ]] || { echo "[k8s-roll] metrics missing new pod $new_pod" >&2; exit 1; }
done

echo "[k8s-roll] PASS old_endpoints_removed old_listeners_closed rollout_complete client=$(cat "$TMP_DIR/client-result.json")"
