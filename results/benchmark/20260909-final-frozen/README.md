# Final frozen benchmark evidence

- Benchmark target commit: `859717ce2cc3160473b391bf18a40194deedbd37`
- Git status at capture: `clean` (recorded in `environment.txt`)
- Build mode: `Release-container`
- Profiles: `workers1-q64`, `workers1-q4096`, `workers4-q64`, `workers4-q4096`
- Fixed controls: `AUTH_WORKER_COUNT=2`, `AUTH_QUEUE_CAPACITY=32`,
  `RESPONSE_QUEUE_CAPACITY=4096`
- Repeats: 3 per cell
- Raw files: 108 across 36 cells, with zero failed requests

Each profile covers 1/10/100/500 clients with 128/4096-byte payloads, plus a
10% slow-reader case with a 50ms read delay. Every raw result records Gateway
`STATS` before/after snapshots and counter deltas; `summary.json` reports
min/median/max across repeats.

The host already had the default ports occupied, so this run used a local-only
Compose override mapping host ports 16379/18080/19000 to the unchanged service
ports 6379/8080/9000. The captured results are local development evidence for
latency, throughput, queue pressure, slow-reader behavior and request
accounting. They are not production capacity claims.
