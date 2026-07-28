# Changelog

## v1.0.0 - 2026-07-28

- Freeze the Phase 0–5 API, Redis, telemetry, and shutdown contracts.
- Engineer the Go control plane with strict HTTP handling, dependency injection,
  graceful shutdown, and Redis recovery-aware readiness.
- Secure token create/list/disable/rotate with generated secrets, HMAC-SHA256
  digests, constant-time comparison, and CAS generation updates.
- Persist gateway status, client snapshots, and versioned runtime configuration
  in Redis with TTL, Pipeline, ETag/If-Match, and Lua CAS semantics.
- Add complete C++ gateway telemetry, atomic dynamic configuration, readiness
  lifecycle, and bounded/classified control-plane HTTP behavior.
- Validate with Go race/vet, CTest, ASan/UBSan, Redis integration tests, Docker
  Compose smoke, and the TCP protocol suite.

Phase 6–9, including Prometheus and Kubernetes, are explicitly outside this
release and are not required for v1.0.0.
