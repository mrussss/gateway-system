# Development Plan

## Goal

Use small, scoped tasks to keep `gateway-system` stable, testable, and easy to review.

This project is already past the "just make it run" phase. The next priority is to avoid broad AI-driven edits that scan the full repo and mix unrelated changes.

## Working Rules

- Do not default to whole-repo scanning.
- Keep each task narrow and file-scoped.
- Prefer plan first, then edit.
- Prefer tests that match the changed area.
- Do not mix protocol work, docs work, CI work, and refactors in one task.

Recommended prompt pattern:

```text
Please output a plan first.
Only inspect files related to this task.
Do not scan the whole repository.
Do not refactor unrelated code.
List:
1. files to inspect
2. files to modify
3. risks
4. tests to run
Wait for confirmation before editing.
```

## Recommended Task Order

```text
1. Confirm the current baseline
2. Review AUTH behavior without editing
3. Add AUTH edge-case tests
4. Fix only issues exposed by review and tests
5. Update docs
6. Add manual smoke CI
```

## Test Scope Rules

Choose the smallest useful validation:

- docs only: no runtime test required
- Go control plane changes: `go test ./...`
- C++ gateway changes: `cmake --build cpp-gateway/build`
- protocol behavior changes: `python3 scripts/tcp_protocol_test.py`
- system integration changes: `bash scripts/smoke_test.sh`
- Docker changes: `docker compose config`

## Current Priorities

- keep the AUTH state machine strict
- keep `/clients` aligned with authenticated state
- keep gateway liveness, Redis-backed status views, and docs aligned
- keep manual smoke coverage working before considering auto CI
- keep README and docs aligned with real behavior

## Current Constraints

- AUTH requires explicit token registration through `POST /tokens`
- Docker Compose uses Redis for tokens, runtime config, gateway status, and clients
- `MemoryStore` still exists for local tests and non-Redis runs
- Docker is required for the repo-level smoke test
- `checkAuth()` is synchronous HTTP, but it already runs in worker threads instead of the epoll thread
- the smoke workflow is manual `workflow_dispatch`, not an every-push CI job
- this is not the phase for broad AI edits, whole-repo refactors, or speculative platform additions

## What Not To Do Yet

- do not refactor the whole `TcpServer`
- do not restructure the repo
- do not add many dependencies
- do not let one task touch many unrelated files
- do not make heavy Docker integration run on every push
