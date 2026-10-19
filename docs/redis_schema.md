# Redis 数据结构

当前 schema 只保存 Token、AUTH 失败和运行配置：

~~~text
token:{client_id}          Hash: digest, generation, created_at, updated_at, disabled
token:index                Set: client ids
auth:failures:{client_id}  String counter with PEXPIRE
config:active              Hash: version and validated runtime fields
~~~

client_id=index 为保留值，避免 Token Hash 与 token:index Set 冲突。禁用不存在的 Token 不会创建空 Hash。

Redis 只保存 HMAC-SHA256 摘要，不保存明文 secret。AUTH 失败计数由 Lua 原子 INCR；只有首次失败设置 TTL，避免后续失败不断延长窗口。认证成功后删除计数。

配置更新通过 Lua CAS 在单次原子操作中比较预期版本、递增版本并写入完整字段。MemoryStore 为测试和本地开发模拟相同行为；不保存业务或网关 Fleet 状态。
