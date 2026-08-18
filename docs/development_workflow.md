# v2 Development Workflow

Gateway System v2 is developed incrementally from the verified v1 baseline.
Implemented behavior is retained when evidence proves the final contract; it is
not rewritten merely because the roadmap names an earlier phase.

Each phase uses a `feature/pN-*` branch and reviewable checkpoints. Before a core
implementation, create the roadmap's annotated checkpoint tag. Each checkpoint
must state its invariant, add or update tests with behavior changes, pass all
checks relevant to its scope, and keep secrets out of source and logs. `main`
must remain buildable. Normal merge commits preserve the learning history.

## Required local checks

```bash
cmake -S cpp-gateway -B cpp-gateway/build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build cpp-gateway/build --parallel
ctest --test-dir cpp-gateway/build --output-on-failure

cmake -S cpp-gateway -B cpp-gateway/build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON \
  -DGATEWAY_ENABLE_SANITIZERS=ON
cmake --build cpp-gateway/build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir cpp-gateway/build-sanitized --output-on-failure

(cd go-control-plane && go test ./...)
(cd go-control-plane && go test -race ./...)
(cd go-control-plane && go vet ./...)

docker compose config
bash -n scripts/*.sh
python3 -m compileall -q scripts cpp-gateway/scripts cpp-gateway/tests
```

Run Redis integration tests for Store changes, Docker smoke for image/protocol or
cross-process changes, Kubernetes rolling-update tests for deployment/drain
changes, and benchmark reproduction for performance claims. If the local
environment lacks Docker or Kubernetes, record that limitation and run the gate
in CI or another named environment before release.

## Fixed delivery sequence

1. Reconcile and freeze v2 scope/contracts.
2. Preserve and regression-test the implemented HTTP, security, Redis CAS/TTL,
   C++ telemetry, dynamic configuration, and shutdown baseline.
3. Complete Prometheus instrumentation and expiry handling.
4. Harden Docker/Compose and automate local failure recovery and CI integration.
5. Add Kubernetes manifests and automated long-connection rolling-update proof.
6. Record reproducible benchmark/fault evidence and pass the complete v2 release
   gate before tagging `v2.0.0`.

The project does not expand into Kafka, SQL databases, Gin/GORM, TLS,
multi-Reactor sharding, service discovery, service mesh, operators, multi-cluster
deployment, a Grafana dashboard, automatic fail-open, or global distributed rate
limiting.

## Release gate

Use `scripts/release_gate.sh --fast` during implementation and
`scripts/release_gate.sh --full` for a release candidate. Full mode includes
every required C++/Go/sanitizer/static check plus real Redis integration,
Docker smoke and recovery, Kubernetes deploy/smoke/rolling update, benchmark
reproduction, and documentation links. It requires caller-provided test secrets
and a reachable cluster, produces no tag, and must be followed by review and
commit of raw environment/results. Only then may an annotated `v2.0.0` tag and
GitHub Release be created.
