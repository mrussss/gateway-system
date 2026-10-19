# 故障语义

| 故障 | 行为 |
| --- | --- |
| 非法帧长度 | 不进入 Worker 队列，记录错误并关闭连接 |
| Normal/AUTH 队列满 | 明确拒绝；业务返回 503，AUTH 尽可能返回 AUTH_OVERLOADED 后关闭 |
| Response 队列满或停止 | 通知 Reactor，仅关闭匹配 fd + conn_id 的连接 |
| 慢读客户端 | 单连接输出受限；超限后关闭 |
| Worker 迟到响应 | fd 与 conn_id 不同时丢弃 |
| 控制面超时、HTTP 格式/状态错误、Go 或 Redis 故障 | AUTH 失败关闭；不终止 C++ 进程 |
| Redis 中断 | readiness 失败；已有数据连接可继续普通请求；新 AUTH 返回 AUTH_UNAVAILABLE；恢复后新调用可成功 |
| 配置获取、解析或版本检查失败 | 保留完整 last-known-good 快照 |
| SIGINT/SIGTERM | C++ 进入有 deadline 的 DRAINING；Go 取消 context 并执行有期限 HTTP shutdown |

## AUTH 拒绝与不可用

Allowed 表示 Go 返回有效 2xx JSON 且 allowed=true；Denied 是可信响应明确 allowed=false；Unavailable 包含 DNS、connect、deadline、I/O、HTTP、JSON、Go 或 Redis 错误。只有 Allowed 才允许通过鉴权，其他结果均失败关闭。

凭据不存在、摘要不匹配或 Token 禁用时增加 auth:failures:{client_id}。非法 JSON、Gateway Token 被拒、Redis 故障、C++ 队列过载或 HTTP 错误不增加失败计数。Redis 原子 INCR 在首次失败时设置 TTL，成功后清除；默认 60 秒、5 次，具体以启动参数为准。

AUTH Worker 和队列容量限制排队量。已断开的任务会尽量在开始前取消；竞态仍由 Reactor 的 fd + conn_id 校验兜底。
