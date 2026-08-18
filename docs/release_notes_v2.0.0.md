# Gateway System v2.0.0

Gateway System v2.0.0 is an incremental release on the verified v1 Phase 0–5
foundation. It preserves the existing C++ TCP data plane, Go control plane,
Redis contracts, token security, runtime configuration, and telemetry design
while completing the Phase 6–9 Prometheus, container, Kubernetes, and release
evidence scope.

## Highlights

- Private Prometheus registry with process/runtime, HTTP, Redis, AUTH,
  configuration, queue, and expiry-aware per-Gateway snapshot metrics.
- Non-root, read-only application images with health-gated Compose startup,
  full protocol smoke coverage, and automated real-Redis outage/recovery.
- Hardened two-replica Kubernetes deployment, Redis StatefulSet/PVC, separate
  liveness/readiness probes, resource limits, PDBs, and bounded graceful drain.
- Authenticated 18-scenario Docker benchmark matrix spanning 1/10/100/500
  clients, two worker/queue profiles, payload sizes, slow readers, CPU/RSS,
  Redis timing, and queue rejection telemetry.
- Deterministic queue saturation, exact fd-reuse, shutdown deadline,
  cancellation, dependency outage, TTL, CAS-conflict, and rolling-update tests.
- Reproducible release workflows, pinned Kind runtime, documentation link
  validation, raw success/failure artifacts, and explicit evidence limits.

## Compatibility and operational notes

- Token creation accepts token metadata and returns the generated secret only
  once; plaintext token material is never accepted for storage or listed later.
- Runtime configuration updates require `If-Match` and use versioned CAS;
  stale writers receive a conflict and cannot silently overwrite a newer value.
- Kubernetes and Compose health checks use the separate `/live` and `/ready`
  contracts. Readiness depends on Redis while liveness does not.
- Gateway shutdown removes readiness, stops accepting, drains accepted work and
  output within the configured deadline, then closes remaining connections.
  Existing connections are not migrated between pods.
- The bundled Redis deployment is a persistent single-instance demonstration,
  not a Redis high-availability topology.

## Validation

The final gate passed clean C++ Debug and ASan/UBSan suites (10/10 each), Go
unit/race/vet tests, real Redis integration, Compose smoke and Redis recovery,
the complete benchmark matrix, manifest validation, two-replica Kind protocol
smoke, and rolling drain. During the rolling test, the reconnecting client
completed 827 requests with six transient failures and a maximum outage of
0.609 seconds, below the 10-second acceptance bound.

The complete decision and run links are in the
[CI release report](../results/release/20260819-ci.md). Raw artifacts and honest
limitations are retained under [`results/`](../results/). These results are
release evidence, not a production capacity, high-availability, or SLO claim.
