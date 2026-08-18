# Kubernetes demonstration deployment

This directory deploys the fixed v2 architecture into namespace
`gateway-system`: two C++ gateway replicas, two Go control-plane replicas, and
one Redis StatefulSet with a PVC. Redis is intentionally a single-replica
demonstration and is not presented as highly available.

## Deploy

Prerequisites are Docker, `kubectl`, and a reachable cluster. For a local Kind
cluster, install `kind` and create the cluster first. Then provide secrets only
through the caller's environment:

```bash
export CONTROL_PLANE_ADMIN_TOKEN='replace-me'
export GATEWAY_SHARED_TOKEN='replace-me'
export TOKEN_PEPPER='replace-me'

bash scripts/k8s_deploy.sh
bash scripts/k8s_smoke.sh
bash scripts/k8s_rolling_update_test.sh
```

The deployment script builds `gateway-system/cpp-gateway:v2-local` and
`gateway-system/control-plane:v2-local` by default and loads them into the
current Kind or Minikube cluster. For a remote registry, set
`K8S_BUILD_IMAGES=0`, `GATEWAY_IMAGE`, and `CONTROL_PLANE_IMAGE` to pullable,
immutable image references.

`secret.example.yaml` documents required keys only. The deploy script creates
the real Secret from environment variables without writing it to the repository
or command output. Do not apply the example as credentials.

## Drain budget and probes

The checked demonstration values are:

```text
preStop signal/propagation window: 3s
Gateway SHUTDOWN_TIMEOUT_MS:       20s
terminationGracePeriodSeconds:    30s
```

The Gateway startup/readiness probes use TCP 9000, so closing the listener makes
the pod unready at the DRAINING boundary. Liveness uses `kill -0 1`, which stays
independent of readiness and therefore does not race the graceful drain. The
preStop hook sends SIGTERM to PID 1 and allows a short endpoint propagation
window; the later Kubernetes SIGTERM is safe because signal handling is
idempotent.

The control plane uses `/health/live` for startup/liveness and `/health/ready`
for readiness. All workloads set requests/limits, run as non-root, drop Linux
capabilities, disallow privilege escalation, use RuntimeDefault seccomp, and
make root filesystems read-only. Only Redis `/data` and explicit `/tmp`
`emptyDir` volumes remain writable.

## Automated evidence

- `scripts/k8s_manifest_test.py` statically checks replica, strategy, security,
  probe, PDB, PVC, and drain-budget invariants.
- `scripts/k8s_smoke.sh` checks two ready gateway replicas, non-root/read-only
  execution, TCP protocol behavior, Prometheus output, and Redis TTL.
- `scripts/k8s_rolling_update_test.sh` runs reconnecting ECHO traffic, restarts
  the Gateway deployment, verifies old endpoint removal and closed listeners,
  waits for rollout completion, checks DRAINING logs and new-pod metrics, and
  rejects a client outage over ten seconds.

These values and checks demonstrate this repository's contract; they are not
universal production sizing advice. Existing TCP connections are drained and
then closed, not migrated to a new pod, so clients must reconnect.
