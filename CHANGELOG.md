# Changelog

## v2.0.0 - Unreleased

- Continue from the verified v1 Phase 0–5 implementation rather than replacing
  the TCP, HTTP, token, Redis, configuration, or telemetry foundations.
- Fix the project scope: Prometheus and Kubernetes rolling updates with graceful
  drain are required v2 deliverables.
- Freeze the v2 API, Redis, metrics, and shutdown contracts before incremental
  Phase 6–9 implementation.

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

This tag remains the historical Phase 0–5 baseline. Phase 6–9 are delivered by
the subsequent v2 roadmap and do not alter the contents of the v1.0.0 tag.
