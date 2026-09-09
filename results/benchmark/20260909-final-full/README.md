# Final controlled benchmark evidence

- Captured from commit `9cdb4e2` (`fix(gate): isolate Redis integration from general Go checks`).
- The benchmark started from a clean Git worktree; see `environment.txt`.
- Docker Compose was run with host ports `16379` (Redis), `18080` (Control Plane), and `19000` (Gateway) because the default Control Plane port was occupied by an unrelated local service.
- All 36 raw JSON results passed with `success=0` failures.

Profiles:

- `workers1-q64`: 1 normal worker, request queue capacity 64
- `workers1-q4096`: 1 normal worker, request queue capacity 4096
- `workers4-q64`: 4 normal workers, request queue capacity 64
- `workers4-q4096`: 4 normal workers, request queue capacity 4096

Every profile includes clients `1`, `10`, `100`, and `500` with payloads `128` and `4096`, plus a 10% slow-client scenario.
