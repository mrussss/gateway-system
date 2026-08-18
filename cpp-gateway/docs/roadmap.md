# Frozen Scope

The job-search version has completed CTest coverage, strict/sanitizer CI, eventfd wakeup, bounded queues, explicit push handling, epoll error coverage, offset output buffers, safe logs, and deadline-bounded shutdown.

The C++ networking architecture remains intentionally frozen against speculative
additions such as multi-Reactor sharding, lock-free queues, coroutines, Kafka,
TLS, or a replacement HTTP stack. Kubernetes deployment and graceful drain are
required integration work for v2 and build on the existing process lifecycle.

Future changes require a reproducible correctness or performance problem, an acceptance test, and updated design evidence. The most plausible measured follow-ups are batching LOG_PUSH disk writes or replacing synchronous Worker AUTH only if profiling shows either path is a real bottleneck.
