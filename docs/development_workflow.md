# Development Workflow

The repository uses AI-assisted development with the same acceptance bar as manually written changes.

Each change should have one explicit problem, a small reviewable diff, stated invariants, automated validation, and documentation updates where behavior changes. Generated code is reviewed at the ownership boundaries: connection state remains Reactor-owned, queue results are handled, blocking calls stay outside Reactor work, and shutdown remains deadline-bounded.

## Required checks

```bash
cmake -S cpp-gateway -B cpp-gateway/build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build cpp-gateway/build --parallel
ctest --test-dir cpp-gateway/build --output-on-failure

(cd go-control-plane && go test ./... && go test -race ./... && go vet ./...)
docker compose config
bash -n scripts/*.sh
python3 -m compileall -q scripts cpp-gateway/scripts cpp-gateway/tests
```

Run the sanitizer build for C++ concurrency, buffer, queue, or lifecycle changes. Run `scripts/smoke_test.sh` for protocol, Docker, Redis, or cross-process changes.

## Scope discipline

The frozen version does not add Kafka, Kubernetes, a dashboard, multi-Reactor sharding, TLS, or a new database. Future work should begin only from a demonstrated correctness, latency, or operability need and include a reproducible acceptance test.
