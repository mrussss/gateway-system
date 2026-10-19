# Gateway System

这是一个 Linux TCP 长连接网关项目：C++17 负责数据面，Go 和 Redis 负责鉴权及版本化运行配置。重点是连接所有权、有界并发、过载处理和有期限的优雅退出。

## 组件职责

- **C++ 网关**：非阻塞 TCP、length-prefixed 协议、单 Reactor + epoll ET；Normal、AUTH、Response 队列均有界；使用 eventfd 唤醒 Reactor，支持部分写、输出背压和连接级 AUTH。
- **Go 控制面**：Token 创建、轮换、禁用与校验；校验运行配置更新；通过 Redis Lua CAS 更新配置版本。
- **Redis**：保存 Token 摘要、AUTH 失败计数和运行配置。MemoryStore 只用于测试和本地开发。

~~~text
TCP Client → C++ Reactor → Normal Queue → Worker
                   │
                   ├→ AUTH Queue → AUTH Worker → Go /auth/check → Redis
                   └← Response Queue ← Workers
                              │ eventfd
                              └→ Reactor

Admin → Go /config → Redis Lua CAS → C++ validated config snapshot
~~~

Reactor 独占 Socket、epoll 状态及连接关闭。异步工作携带 fd 与递增的 conn_id；连接关闭并发生 fd 复用后，旧响应会被丢弃。

## 启动

~~~bash
cp .env.example .env
docker compose up --build
~~~

Compose 默认映射 Redis 6379、Go 控制面 8080、C++ 网关 9000。生产式配置须显式提供管理 Token、网关共享 Token 和 pepper；不要把真实凭据提交到仓库。

## 验证

快速 gate：

~~~bash
scripts/release_gate.sh --fast
~~~

完整 gate 需要 Docker、真实 Redis 和测试凭据，另运行 Compose Smoke、Redis 中断恢复和完整 Benchmark：

~~~bash
CONTROL_PLANE_ADMIN_TOKEN=... GATEWAY_SHARED_TOKEN=... TOKEN_PEPPER=... \
  scripts/release_gate.sh --full
~~~

Full Gate 会清理自己的 Compose 项目及其数据卷；运行前先核对项目名、端口和数据卷。CI、单独 Smoke、Benchmark 和 Full Gate 覆盖不同场景，不能互相替代。

## 阅读入口

- [系统架构](docs/architecture.md)、[协议](docs/protocol.md)、[HTTP API](docs/api_contract.md)
- [Redis 结构](docs/redis_schema.md)、[连接生命周期](docs/connection_lifecycle.md)、[退出契约](docs/shutdown.md)
- [故障语义](docs/failure_semantics.md)、[测试说明](docs/testing.md)、[Benchmark](docs/benchmark.md)

源码入口：cpp-gateway/src/net/TcpServer.cpp、cpp-gateway/src/protocol/、go-control-plane/internal/app/。

## 项目边界

单 Reactor 简化连接状态所有权；不承诺持续输入下的调度公平性。队列和每连接输出有上限，过载时拒绝或关闭慢连接。配置每 5 秒拉取，无效更新保留 last-known-good。连接数与速率限制只在单进程内生效。同步 getaddrinfo 不受 Socket I/O deadline 控制。项目不提供多节点 Redis 高可用、网关集群管理、TLS 终止、Prometheus 产品监控或 Kubernetes 运维。
