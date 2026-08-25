# Frozen Scope

The job-search version has completed CTest coverage, strict/sanitizer CI, eventfd wakeup, bounded queues, explicit push handling, epoll error coverage, offset output buffers, safe logs, and deadline-bounded shutdown.

The C++ networking architecture remains intentionally frozen against speculative
additions such as multi-Reactor sharding, lock-free queues, coroutines, Kafka,
TLS, or a replacement HTTP stack. Graceful drain is already part of the final
scope; there is no deployment-platform roadmap.

Future changes are limited to bugs, security issues, test fixes and interview
feedback. No new product surface is planned.
