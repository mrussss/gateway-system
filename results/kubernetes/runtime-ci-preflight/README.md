# Superseded Kubernetes preflight artifact

Run `32160645121` proved the rolling-update path itself: both old listeners were
closed, DRAINING and clean shutdown were logged, the reconnecting client
completed 851 ECHO requests with three transient failures and two connections,
and maximum outage was 0.609 seconds.

It is retained as failure evidence, not a passing release gate. Its preceding
`k8s_smoke.sh` failed because the protocol suite queried the single-instance
legacy `/clients` view in a two-Gateway deployment and could observe the other
Gateway's snapshot. The workflow step piped through `tee` without `pipefail`, so
GitHub incorrectly marked the overall run successful. Commit `abaa8b4` changed
the suite to aggregate `/gateways/{gateway_id}/clients` and made every evidence
pipeline propagate the tested command's exit status. The final rerun belongs in
the sibling `runtime-ci/` directory.
