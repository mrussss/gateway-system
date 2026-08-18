# Docker smoke and Redis recovery — GitHub Actions 32161089687

Run `https://github.com/mrussss/gateway-system/actions/runs/32161089687`
executed candidate `abaa8b4f04ae1899fa46afb2cce2f88f77e279af` on a
GitHub-hosted Azure Linux runner with 4 logical CPUs, 15 GiB RAM, Docker 28.0.4,
GCC 13.3.0, and Go 1.24.13. The exact machine capture is `environment.txt`.

Commands:

```bash
bash scripts/smoke_test.sh
bash scripts/redis_recovery_test.sh
```

Both commands ran through `tee` with `pipefail` enabled and the workflow passed.
`smoke.log` contains the full image builds, non-root/read-only checks,
live/ready, gateway/client views, Prometheus parser surface, Redis TTL, one-time
token create/list/rotate/disable, config ETag CAS/conflict, and TCP protocol
suite; line 862 records `[smoke] PASS`.

`redis-recovery.log` pauses the real Redis 7.4.10 container and proves live=200,
ready=503, established authenticated ECHO continuity, fail-closed new AUTH,
then automatic ready/AUTH/report recovery and config-version retention after
unpause. Line 149 records the final PASS.

This is short single-host Compose evidence with local bridge networking and
ephemeral test secrets. It proves the checked fault contract, not Redis high
availability, durability under host loss, TLS, or production capacity.
