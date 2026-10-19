# 系统架构

Gateway System 由 Linux C++ 数据面和窄范围 Go/Redis 控制面组成。MemoryStore 只作为测试与本地开发替身。

## TCP 数据路径

~~~text
TCP Client → C++ Reactor
                 ├── Normal Queue → Worker ─┐
                 ├── AUTH Queue → AUTH Worker ── Go /auth/check → Redis
                 └── Response Queue ◄────────┘
                              │ eventfd
                              └───────────────→ Reactor
~~~

Reactor 独占 accept、读写、epoll 状态和连接关闭。Worker 不修改 Socket 生命周期。请求和响应携带 fd + conn_id，连接编号防止内核复用 fd 后误用旧响应。

收到 FIN 后先排空可读数据，再标记 read_eof。完整帧照常接纳，残缺尾帧记录后丢弃。只要还有 in-flight 工作或待发送输出，连接继续保持可写。

Normal、AUTH、Response 队列均有界。独立 AUTH Worker 避免 Go/Redis 延迟占用普通 Worker。响应入队后以 eventfd 唤醒 Reactor，避免固定周期轮询。

## 背压与退出

每连接输出缓冲使用 write offset 处理部分写；待发数据保留并启用 EPOLLOUT。达到输出上限时关闭慢读连接。生命周期为 RUNNING → DRAINING → STOPPED。DRAINING 停止接纳新任务，排空已接纳工作与输出，到 deadline 后中止剩余任务并关闭连接。

## 控制路径

~~~text
AUTH Worker → POST /auth/check → Redis Token 摘要与失败 TTL
Admin → PUT /config + If-Match → Redis Lua CAS
      → 版本递增 → C++ 拉取 → 校验 → 不可变快照切换
~~~

Token 用安全随机源生成，Redis 只存 HMAC-SHA256 摘要。AUTH 失败计数通过原子 INCR 与 TTL 保存；成功后清除，Redis 故障时失败关闭。C++ 只应用校验通过的较新配置，失败时保留 last-known-good。

## 项目边界

不包含 Kubernetes、Prometheus 产品监控、多网关注册、客户端汇总、服务发现、分布式限流、TLS 终止、业务数据库、消息队列、多 Reactor 或 Redis 高可用。单 Reactor 不承诺持续输入下的调度公平性；同步 getaddrinfo 不受 Socket deadline 控制。
