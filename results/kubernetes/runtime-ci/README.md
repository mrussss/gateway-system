# Kubernetes smoke and rolling drain — GitHub Actions 32161089464

Run `https://github.com/mrussss/gateway-system/actions/runs/32161089464`
executed candidate `abaa8b4f04ae1899fa46afb2cce2f88f77e279af` in a
pinned Kind v0.32.0 cluster using Kubernetes v1.35.5. The GitHub Azure runner
had 4 logical x86-64 CPUs, 15 GiB RAM, kernel `6.17.0-1022-azure`, Docker
28.0.4, GCC 13.3.0, and Go 1.24.13; `environment.txt` is the raw capture.

Commands:

```bash
bash scripts/k8s_deploy.sh
bash scripts/k8s_smoke.sh
bash scripts/k8s_rolling_update_test.sh
```

All commands ran through `tee` with `pipefail` enabled. `smoke.log` proves two
ready Gateway replicas, non-root/read-only security, the full TCP protocol
suite, aggregated multi-Gateway client snapshots, Prometheus output, and a
positive Redis TTL. It ends with `[k8s-smoke] PASS`.

The rolling test captured both old pods, sent continuous authenticated ECHO,
restarted the Deployment, observed endpoint removal and closed old listeners,
and required a successful rollout plus metrics for every new pod.
`gateway-rollout.log` contains `graceful shutdown started`, `drain completed`,
and `shutdown complete` for both old pods.

`client-result.json` is the raw reconnecting-client result:

```json
{"successes": 827, "failures": 6, "connections": 3, "max_outage_seconds": 0.609}
```

The 0.609-second maximum outage is below the automated 10-second acceptance
bound. Existing connections were closed and re-established, not migrated.

This is a single-node Kind demonstration with one non-HA Redis pod, ephemeral
CI storage, no TLS, and a short 45-second client. It validates the checked
rolling-drain contract, not multi-node failure, Redis HA, a zero-disconnect
guarantee, or production availability.
