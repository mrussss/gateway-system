# Redis schema

The final Redis schema contains only token, AUTH failure and runtime-config
state:

    token:{client_id}       Hash: digest, generation, created_at, updated_at, disabled
    token:index              Set: client ids
    auth:failures:{client_id} String counter with PEXPIRE
    config:active            Hash: version and validated runtime fields

Token digests are the only credential material stored. AUTH failure increments
use a Lua script so the first increment sets the window TTL atomically.
Configuration updates use a Lua compare-and-set: expected version is checked,
the version is incremented and the complete new snapshot is written in one
operation.

MemoryStore mirrors these semantics for tests and local development. It does
not implement a separate business or fleet model.
