# 后续开发计划

## 近期优化（V5）

### 1. eventfd 响应唤醒

**现状**：Worker 线程处理完请求后将 Response 推入 `response_queue_`，但主事件循环只有在下一次 `epoll_wait` 返回后才会 drain 该队列。`epoll_wait` timeout 为 100ms，导致低并发场景下引入约 100ms 的额外延迟。

**目标**：使用 `eventfd` 让 Worker 线程在推入响应后主动唤醒 epoll，使响应可以被立即处理。

**实现方向**：

```
Worker 推入 response_queue_
  → eventfd 写入 (write)
  → epoll_wait 立即返回
  → drainResponseQueue()
  → 发送响应
```

### 2. 连接读写缓冲区优化

**现状**：input_buffer 和 output_buffer 使用 `std::string`，通过 `erase(0, N)` 清理已处理数据。这种方式不会释放已分配的内存，长连接大流量场景下可能累积不必要的 capacity。

**方向**：
- 使用环形缓冲区（ring buffer）替代字符串
- 或定期 shrink buffer
- 或引入内存池复用缓冲区

### 3. 连接级别水位线

**现状**：output_buffer 超过 2MB 后直接关闭连接。

**方向**：引入高低水位（watermark）机制：

```
output_buffer > high_watermark  → 暂停 EPOLLIN，不读该连接新数据
output_buffer < low_watermark   → 恢复 EPOLLIN
```

这样可以在不关闭连接的情况下对慢客户端施加背压。

### 4. 更完善的服务端监控指标

**现状**：STATS 已提供基本统计（请求数、错误数、连接数、流量）。

**方向**：
- 按消息类型分别统计（PING/ECHO/LOG_PUSH/STATS）
- Worker 线程处理延迟统计
- 队列积压时间统计
- 内存使用概览

---

## 中期规划

### 5. Go 管理服务

开发一个 Go 编写的 HTTP 管理接口服务，与服务端配合部署：

```
┌──────────────┐      ┌──────────────────┐
│  Go 管理服务   │─────▶│  MySQL / Redis   │
│  (HTTP API)   │      │  (状态存储)      │
├──────────────┤      └──────────────────┘
│  /admin/...  │
│  /api/...    │
└──────┬───────┘
       │
       │ 内部 RPC / 共享队列
       ▼
┌──────────────┐
│  MessageServer│
│  (C++ 接入层) │
└──────────────┘
```

管理服务功能：
- 设备管理（绑定/解绑）
- 在线状态查询
- 消息记录查询
- 鉴权（JWT）
- 实时监控看板

### 6. MySQL / Redis 状态存储

**现状**：日志数据写入本地文件，无持久化数据库。

**方向**：
- Redis 存储在线设备列表、会话状态
- MySQL 存储消息记录、设备绑定信息、操作日志
- Go 管理服务作为数据层的统一入口

### 7. JWT 鉴权

在 TCP 握手或首次消息中引入 JWT 鉴权：

```
客户端连接 → 发送 AUTH 消息（含 JWT token）
服务端校验 token → 成功则标记连接为已验证
未验证的连接在一定时间后被关闭
```

JWT 的验证可以在 Go 管理服务中完成，C++ 服务端只负责透传和缓存验证结果。

### 8. 消息记录查询

LOG_PUSH 上报的日志消息除了写入本地文件外，可同时写入存储层，通过管理服务提供查询接口：

```
Python 客户端 → LOG_PUSH → C++ 服务端
  → 写入 access.log（本地持久化）
  → 写入 Kafka / Redis 队列
    → Go 管理服务消费
      → 写入 MySQL（可查询）
```

---

## 长期目标

### 9. 多 Reactor 模型

**现状**：单 Reactor（一个主线程处理所有网络事件 + IO），Worker 线程池处理业务。

**方向**：主 Reactor 只负责 accept，多个 Sub Reactor 负责连接 IO：

```
Main Reactor（1 个线程）
  → accept 新连接
  → 分发给 Sub Reactor

Sub Reactor（N 个线程）
  → 负责一组连接的 read / write
  → 每个 Sub Reactor 有自己的 epoll fd

Worker Pool（M 个线程）
  → 从共享队列取 Request
  → 处理后放入对应 Sub Reactor 的 Response 队列

Response 返回：
  Worker → Sub Reactor 的 response_queue
    → eventfd 唤醒 Sub Reactor
    → Sub Reactor drain 并发送
```

### 10. 更高性能压测

使用 C++ 或 Go 编写 benchmark 客户端，消除 Python GIL 和线程模型在高并发场景下的客户端瓶颈。

### 11. 多协议支持

在 TCP 二进制协议基础上，考虑支持 WebSocket 接入，方便浏览器客户端直接连接。

---

## 时间线（参考）

| 阶段 | 内容                                | 预估   |
| ---- | ----------------------------------- | ------ |
| V5.0 | eventfd 唤醒、Buffer 优化、水位线   | 1-2 周 |
| V5.1 | 监控指标完善、性能压测              | 1 周   |
| V6.0 | Go 管理服务设计 + 基础 API          | 2-3 周 |
| V6.1 | MySQL/Redis 状态存储                | 2 周   |
| V6.2 | JWT 鉴权                            | 1 周   |
| V7.0 | 多 Reactor 重构                     | 3-4 周 |
| V7.1 | 消息记录查询（LOG_PUSH 持久化链路） | 2 周   |

---

## 不做（明确不纳入规划）

- 自研 HTTP 解析器：不重复造轮子，需要 HTTP 时用 Go 管理服务提供
- 全异步日志库：当前同步写入在目标规模下够用
- TLS/SSL 支持：暂不需要，按需引入
- 自定义配置中心：暂不需要，建议环境变量或简单配置文件
