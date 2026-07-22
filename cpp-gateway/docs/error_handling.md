# Error Handling

| Failure | Implemented behavior |
| --- | --- |
| incomplete frame | retain bytes and wait for the next EPOLLIN |
| invalid body length | increment error and close connection |
| Request Queue full | status 503; AUTH closes after response |
| Request Queue stopped | reject explicitly; never silently drop |
| Response Queue full/stopped | count error, notify Reactor independently, close matching fd + conn_id |
| output above 8 MiB | close the slow connection |
| stale response | discard when conn_id does not match |
| EPOLLERR/HUP/RDHUP | close through the common connection path |
| EINTR | retry accept4/recv/send/eventfd/control HTTP IO |
| config fetch/parse failure | retain current complete snapshot |
| control plane unavailable during AUTH | fail closed |
| shutdown deadline | force-close remaining connections |

`BlockQueue::stop()` rejects new pushes while preserving drain semantics for items already present. Consumers return false only after the stopped queue becomes empty.
`BlockQueue::abort()` is used only after the shutdown deadline and discards pending elements.

Detailed cross-component failures are documented in [failure cases](../../docs/failure_cases.md).
