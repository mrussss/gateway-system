# Evidence

Results are reserved for reproducible evidence of the permanent Final Scope:
C++ build/sanitizer/benchmark/failure cases, Go tests, real Redis contracts,
Compose smoke and recovery.

The repository does not retain evidence for removed Prometheus, multi-gateway,
online-client aggregation or Kubernetes functionality. Run the current gate to
create fresh local artifacts:

    scripts/release_gate.sh --fast
    scripts/release_gate.sh --full
