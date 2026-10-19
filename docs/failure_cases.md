# 故障场景索引

当前完整故障语义统一记录在[故障语义](failure_semantics.md)，避免协议、AUTH、队列、Redis 和退出行为发生文档漂移。

测试覆盖 Redis 不可用、AUTH fail-closed、配置保留、队列过载、迟到响应、慢读客户端和 deadline 退出；见[测试说明](testing.md)。
