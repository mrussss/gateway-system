# Kubernetes runtime evidence

## Local WSL status — pending

On 2026-08-19 the manifest contract passed with:

```bash
python3 scripts/k8s_manifest_test.py
```

This is static validation only. The local environment reported `kubectl:
unavailable`; Kind v0.32.0 was installed, but Docker Desktop WSL integration was
disabled, so no cluster could be created and no rolling update was claimed.

Before `v2.0.0`, run the following in a Docker- and Kubernetes-enabled
environment and add the environment capture plus raw command logs here:

```bash
scripts/capture_environment.sh results/kubernetes/environment.txt
bash scripts/k8s_deploy.sh 2>&1 | tee results/kubernetes/deploy.log
bash scripts/k8s_smoke.sh 2>&1 | tee results/kubernetes/smoke.log
bash scripts/k8s_rolling_update_test.sh 2>&1 | tee results/kubernetes/rolling-update.log
```

Required proof is two ready Gateway replicas, non-root/read-only execution,
protocol and metrics success, positive Redis TTLs, old endpoint/listener
removal, DRAINING logs, reconnecting ECHO outage no greater than ten seconds,
and successful `kubectl rollout status`.

The manual `Kubernetes rolling update` GitHub workflow creates a pinned Kind
v0.32.0/Kubernetes v1.35.5 cluster, runs the same deploy/smoke/rolling scripts,
and uploads environment, command, reconnecting-client, forwarding, and cluster
logs as the `kubernetes-runtime-evidence` artifact. The node image is pinned by
digest so a rerun does not silently change Kubernetes bits. A successful
artifact still needs to be copied into this directory before release so the tag
is self-contained.
