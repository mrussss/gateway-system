# Current State

This document freezes the v2 development baseline at commit `79bb8ad` on 2026-07-28.

The baseline provides a C++17 single-Reactor TCP gateway, bounded worker queues,
an eventfd notifier, a Go control plane, Redis-backed storage, Docker Compose,
and CTest/ASan/UBSan/Go race coverage. It is not a blank project and must be
extended without replacing the TCP server.

## Fixed v2 scope

- C++ long-connection data plane and Go standard-library control plane.
- Redis for token, gateway-state, and configuration storage.
- Prometheus observability and Kubernetes rolling-drain validation.

## Explicitly out of scope

- Kafka, a relational database, Gin, multi-Reactor sharding, TLS, service mesh,
  reverse proxying, and transparent TCP connection migration.

The contracts in this directory are the implementation source of truth for v2.
