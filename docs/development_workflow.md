# Development workflow

## Local checks

Run focused checks while editing:

    (cd go-control-plane && gofmt -w internal/app/*.go cmd/control-plane/*.go)
    (cd go-control-plane && go test ./...)
    cmake -S cpp-gateway -B cpp-gateway/build -DCMAKE_BUILD_TYPE=Debug
    cmake --build cpp-gateway/build --parallel
    ctest --test-dir cpp-gateway/build --output-on-failure

Use go test -race ./... and go vet ./... before committing Go changes. Use the
sanitized C++ build for lifetime, bounds and undefined-behavior changes.

## Release gates

scripts/release_gate.sh --fast runs C++ build/CTest, ASan/UBSan, Go
test/race/vet, Compose syntax, shell/Python syntax and Markdown link checks.

--full adds real Redis token/AUTH/config contracts, Compose smoke, Redis
pause/recovery semantics and the authenticated TCP benchmark matrix. A missing
Docker or Redis runtime is recorded as SKIPPED, never treated as a pass.

## Scope discipline

Do not add a new framework or infrastructure product. Keep C++ socket lifetime
owned by the Reactor, keep normal and AUTH work isolated, preserve queue
capacity semantics and retain last-known-good configuration on failed fetches.
Changes outside bugs, security, tests and interview feedback require an explicit
scope decision because the project is frozen.
