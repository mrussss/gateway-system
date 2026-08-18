# Reproducible evidence

This tree contains raw evidence and the environment needed to interpret it. A
report is evidence only when it identifies the exact command, build mode,
backend, workload, and limitations. Missing runtime access is recorded as a
pending gate; a static manifest check is never presented as a cluster run.

## Layout

- `environment/`: machine, kernel, CPU, memory, toolchain, container, and
  cluster capture.
- `benchmark/`: raw benchmark JSON plus a human-readable run report.
- `failures/`: fault-injection matrix mapped to executable tests and evidence.
- `kubernetes/`: cluster smoke and rolling-update reports. The local report is
  pending because this WSL environment has neither `kubectl` nor an enabled
  Docker daemon.

Capture a new environment and run the complete container benchmark matrix:

```bash
scripts/capture_environment.sh
scripts/benchmark_matrix.sh
```

Run `scripts/release_gate.sh --help` for the staged release checks. Raw JSON and
logs should be committed together with the report that references them; secrets
and generated `.env` files must never be committed.
