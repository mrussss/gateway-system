# Current State

This document records the completed v1.0.0 scope on 2026-07-28.

The baseline provides a C++17 single-Reactor TCP gateway, bounded worker queues,
an eventfd notifier, a Go control plane, Redis-backed storage, Docker Compose,
and CTest/ASan/UBSan/Go race coverage.

## Completed v1.0.0 scope

- C++ long-connection data plane and Go standard-library control plane.
- Redis for token, gateway-state, and configuration storage.
- Phase 0–5 contracts, HTTP engineering, token security, Redis state/config CAS,
  and C++ telemetry/dynamic configuration.

## Explicitly out of scope

- Phase 6–9, Prometheus, Kubernetes, Kafka, a relational database, Gin,
  multi-Reactor sharding, TLS, service mesh, reverse proxying, and transparent
  TCP connection migration.

The contracts in this directory are the implementation source of truth for
v1.0.0. Further feature development requires a separately approved scope.
