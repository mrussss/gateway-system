# Frozen Scope

The job-search version has completed CTest coverage, strict/sanitizer CI, eventfd wakeup, bounded queues, explicit push handling, epoll error coverage, offset output buffers, safe logs, and deadline-bounded shutdown.

The repository is intentionally frozen against speculative additions such as multi-Reactor sharding, lock-free queues, coroutines, Kafka, Kubernetes, TLS, or a replacement HTTP stack.

Future changes require a reproducible correctness or performance problem, an acceptance test, and updated design evidence. The most plausible measured follow-ups are batching LOG_PUSH disk writes or replacing synchronous Worker AUTH only if profiling shows either path is a real bottleneck.
