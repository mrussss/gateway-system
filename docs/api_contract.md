# HTTP API 契约

Go 控制面默认监听 8080。JSON 请求使用 Content-Type: application/json；未知字段、尾随 JSON 值和超限请求体会被拒绝。响应包含 Request ID、Content-Length 和结构化 JSON。

## 认证与健康检查

管理接口使用 Authorization: Bearer <CONTROL_PLANE_ADMIN_TOKEN>；C++ 网关到 Go 使用 X-Gateway-Token: <GATEWAY_SHARED_TOKEN>。

| 方法 | 路径 | 认证 | 说明 |
| --- | --- | --- | --- |
| GET | /health | 无 | HTTP 进程正在服务 |
| GET | /health/ready | 无 | Store 可用并能接受工作 |
| POST | /auth/check | Gateway Token | 校验 client_id 和 token |
| POST | /tokens | 管理 Token | 创建 Token，返回一次性 secret |
| GET | /tokens | 管理 Token | 仅列出元数据 |
| DELETE | /tokens/{client_id} | 管理 Token | 禁用 Token；不存在返回 404 |
| POST | /tokens/{client_id}/rotate | 管理 Token | 用 If-Match generation 轮换 |
| GET | /config | 管理或 Gateway Token | 读取配置和 ETag |
| PUT | /config | 管理 Token | 用 If-Match 原子更新配置 |

Store 不可用时 readiness 返回 503 NOT_READY。Token secret 只在创建时返回；存储仅保留 HMAC-SHA256 摘要。client_id 限 1–128 字节，index 为 Redis 索引集合保留值。

AUTH 的业务拒绝返回 HTTP 200 及 allowed/code。依赖故障返回 HTTP 503 AUTH_UNAVAILABLE，调用方失败关闭。只有确认的凭据错误增加失败计数；计数带 TTL，成功认证后清除。

## 运行配置

PUT /config 必须携带当前 ETag 对应的 If-Match。版本冲突返回 409 CONFLICT，缺少前置条件返回 428 PRECONDITION_REQUIRED。Redis Lua CAS 负责比较并递增版本。C++ 只接受完整校验且版本更新的配置；拉取或校验失败时保留 last-known-good。

字段包括 version、max_payload_size、max_connections_per_client、max_requests_per_client_per_second、slow_client_output_limit、log_level、request_queue_capacity_display。queue capacity display 仅供显示，实际队列容量由 C++ 启动参数控制。

max_payload_size 沿用现有字段名，但限制的是完整协议 body（包括 10 字节固定头），范围为 10 到 4 MiB + 10 字节。slow_client_output_limit 还要容纳 4 字节长度前缀，且不能超过 64 MiB。连接数和每客户端每秒请求数范围为 1 到 2147483647。Go 与 C++ 均拒绝小数及越界整数；C++ 校验失败时保留当前快照。
