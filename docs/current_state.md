# 当前项目范围

仓库维护 C++17 Linux TCP 数据面，以及 Go/Redis Token、AUTH 和版本化配置控制面。

## 已有能力

- C++：非阻塞 TCP、epoll ET、单 Reactor、二进制 framing、有界 Normal/AUTH/Response 队列、eventfd、部分写、背压、fd + conn_id 校验及有期限退出。
- Go/Redis：Token 生命周期、AUTH 失败计数和 TTL、配置校验与 Lua CAS、单调版本、健康检查与退出。
- 验证：CTest、ASan/UBSan、Go unit/race/vet、真实 Redis、Compose Smoke/恢复和认证 TCP Benchmark。

## 不在当前范围

Kubernetes、Prometheus 产品监控、多网关注册、在线客户端汇总、服务发现、分布式限流、TLS 终止、业务数据库、消息队列、多 Reactor 和 Redis 高可用。

维护范围为 Bug、安全修复、测试修复及面试反馈。历史 v2 实现可从 Git 分支 archive/v2-full 和标签 v2-full-archive 读取。
