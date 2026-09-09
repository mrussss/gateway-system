# Final controlled benchmark evidence

- Benchmark commit: `77c2d14a01b6c487c67837b8e345d628efdfd677`
- Git status at capture: `clean`
- Build mode: `Release-container`
- Profiles: `workers1-q64`, `workers1-q4096`, `workers4-q64`, `workers4-q4096`
- Fixed controls: `AUTH_WORKER_COUNT=2`, `AUTH_QUEUE_CAPACITY=32`,
  `RESPONSE_QUEUE_CAPACITY=4096`
- Cases: 1/10/100/500 clients, 128/4096-byte payloads, plus 10% slow readers
  with a 50ms read delay
- Raw files: 36

The host already had port 8080 occupied, so this run used a local-only Compose
override mapping host ports 16379/18080/19000 to the unchanged service ports
6379/8080/9000. The endpoint override is recorded by each raw JSON result.

These files are development evidence for latency, throughput, queue pressure,
slow-reader behavior and request accounting. They are not production capacity
claims.
