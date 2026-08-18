#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
NAMESPACE="gateway-system"
GATEWAY_IMAGE="${GATEWAY_IMAGE:-gateway-system/cpp-gateway:v2-local}"
CONTROL_PLANE_IMAGE="${CONTROL_PLANE_IMAGE:-gateway-system/control-plane:v2-local}"

require_command() {
  command -v "$1" >/dev/null 2>&1 || { echo "[k8s-deploy] missing command: $1" >&2; exit 1; }
}

require_secret() {
  local name="$1"
  [[ -n "${!name:-}" ]] || { echo "[k8s-deploy] $name must be set" >&2; exit 1; }
}

require_command kubectl
require_secret CONTROL_PLANE_ADMIN_TOKEN
require_secret GATEWAY_SHARED_TOKEN
require_secret TOKEN_PEPPER
cd "$ROOT_DIR"

if [[ "${K8S_BUILD_IMAGES:-1}" == "1" ]]; then
  require_command docker
  docker build -t "$GATEWAY_IMAGE" cpp-gateway
  docker build -t "$CONTROL_PLANE_IMAGE" go-control-plane

  context="$(kubectl config current-context)"
  if [[ "$context" == kind-* ]] && command -v kind >/dev/null 2>&1; then
    cluster_name="${context#kind-}"
    kind load docker-image --name "$cluster_name" "$GATEWAY_IMAGE" "$CONTROL_PLANE_IMAGE"
  elif [[ "$context" == minikube ]] && command -v minikube >/dev/null 2>&1; then
    minikube image load "$GATEWAY_IMAGE"
    minikube image load "$CONTROL_PLANE_IMAGE"
  else
    echo "[k8s-deploy] Images were built locally; ensure this cluster can pull or access them."
  fi
fi

kubectl apply -f deploy/kubernetes/namespace.yaml
kubectl -n "$NAMESPACE" create secret generic gateway-system-secrets \
  --from-literal=admin-token="$CONTROL_PLANE_ADMIN_TOKEN" \
  --from-literal=gateway-token="$GATEWAY_SHARED_TOKEN" \
  --from-literal=token-pepper="$TOKEN_PEPPER" \
  --dry-run=client -o yaml | kubectl apply -f -

kubectl apply -f deploy/kubernetes/configmap.yaml
kubectl apply -f deploy/kubernetes/redis-headless-service.yaml
kubectl apply -f deploy/kubernetes/redis-statefulset.yaml
kubectl -n "$NAMESPACE" rollout status statefulset/redis --timeout=180s

kubectl apply -f deploy/kubernetes/control-plane-service.yaml
kubectl apply -f deploy/kubernetes/gateway-service.yaml
kubectl apply -f deploy/kubernetes/control-plane-pdb.yaml
kubectl apply -f deploy/kubernetes/gateway-pdb.yaml
kubectl apply -f deploy/kubernetes/control-plane-deployment.yaml
kubectl apply -f deploy/kubernetes/gateway-deployment.yaml
kubectl -n "$NAMESPACE" set image deployment/control-plane control-plane="$CONTROL_PLANE_IMAGE"
kubectl -n "$NAMESPACE" set image deployment/gateway gateway="$GATEWAY_IMAGE"
kubectl -n "$NAMESPACE" rollout status deployment/control-plane --timeout=180s
kubectl -n "$NAMESPACE" rollout status deployment/gateway --timeout=180s
kubectl -n "$NAMESPACE" get pods -o wide

echo "[k8s-deploy] PASS namespace=$NAMESPACE gateway_image=$GATEWAY_IMAGE control_plane_image=$CONTROL_PLANE_IMAGE"
