# Gateway System v2.0.1

This patch completes the final requirement-by-requirement audit of the v2
development plan without replacing or restructuring the existing system.

## Corrections

- Unknown routes and unsupported methods now use the same JSON error envelope
  and Request ID as every other API failure. HTTP 405 also includes `Allow`.
- Redis connection and deadline failures now return HTTP 503 `UNAVAILABLE`;
  malformed retained data and other internal Store failures remain HTTP 500.
- Requests rejected by body/content middleware are included in bounded-label
  HTTP metrics and structured access logs.
- Direct regression tests cover all three paths, including an actual refused
  Redis connection.

## Verification

- C++ Debug CTest: 10/10.
- C++ ASan/UBSan CTest: 10/10.
- Go unit, race, vet, module, and real Redis tests: passed.
- Docker protocol smoke and Redis pause/recovery: run
  [32163238255](https://github.com/mrussss/gateway-system/actions/runs/32163238255).
- Pinned Kind protocol smoke and rolling drain: run
  [32163245109](https://github.com/mrussss/gateway-system/actions/runs/32163245109).
- 18-scenario Docker/Redis benchmark: run
  [32163250493](https://github.com/mrussss/gateway-system/actions/runs/32163250493).

The complete Phase 0–9 and 30-criterion mapping is in the
[completion audit](../results/release/20260819-completion-audit.md). Existing
limits remain unchanged: this is not a TLS, Redis-HA, zero-disconnect,
production-capacity, or SLO claim.
