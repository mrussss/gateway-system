# 项目架构设计

## 1. 项目定位

MessageServer 是一个基于 Linux epoll（边缘触发模式）和非阻塞 socket 实现的轻量级 TCP 长连接消息接入服务。它接收客户端通过自定义二进制协议发送的消息，进行协议解析、业务分发、响应回写和可选的日志持久化。

项目不依赖第三方网络库，核心网络层和协议层均为手写实现，适合用于学习服务端网络编程，也可作为消息接入服务的基础框架。

---

## 2. 整体架构

```
┌──────────────────────────────────────────────────────────────┐
│                       客户端 (Client)                         │
│               Python 测试脚本 / 其他 TCP 客户端                │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           │  TCP 长连接 (自定义二进制协议)
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                    Epoll 事件循环 (主线程)                      │
│                                                              │
│   ┌─────────────┐  ┌─────────────┐  ┌───────────────────┐   │
│   │ handleAccept│  │  handleRead │  │   handleWrite     │   │
│   │ (accept 新  │  │  (recv 数据 │  │  (send 响应数据)   │   │
│   │  连接)      │  │   + 解码)   │  │                   │   │
│   └──────┬──────┘  └──────┬──────┘  └────────┬──────────┘   │
│          │                │                   │              │
│          ▼                ▼                   ▼              │
│   ┌──────────────────────────────────────────────────┐      │
│   │              Connection 连接管理器                 │      │
│   │     unordered_map<int, Connection> connections_   │      │
│   │    每个连接维护: fd, conn_id, input/output buffer  │      │
│   └──────────────────────────────────────────────────┘      │
│                            │                                 │
│                            ▼                                 │
│   ┌──────────────────────────────────────────────────┐      │
│   │           ProtocolCodec::decode()                 │      │
│   │     粘包/半包处理, 协议校验, 生成 Request          │      │
│   └──────────────────────┬───────────────────────────┘      │
│                          │                                   │
│                          ▼                                   │
│   ┌──────────────────────────────────────────────────┐      │
│   │           BlockQueue<Request> request_queue_      │      │
│   │              (线程安全阻塞队列)                    │      │
│   └──────────────────────┬───────────────────────────┘      │
└──────────────────────────┼───────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                  Worker 线程池 (2~4 个线程)                    │
│                                                              │
│   ┌──────────────────────────────────────────────────┐      │
│   │               Dispatcher::dispatch()              │      │
│   │    根据 MessageType 分发到具体的 Handler 处理      │      │
│   │    PING → handlePing()                           │      │
│   │    ECHO → handleEcho()                           │      │
│   │    LOG_PUSH → handleLogPush()                    │      │
│   │    STATS → handleStats()                         │      │
│   │    default → makeErrorResponse()                 │      │
│   └──────────────────────┬───────────────────────────┘      │
│                          │                                   │
│                          ▼                                   │
│   ┌──────────────────────────────────────────────────┐      │
│   │           BlockQueue<Response> response_queue_    │      │
│   │              (线程安全阻塞队列)                    │      │
│   └──────────────────────┬───────────────────────────┘      │
└──────────────────────────┼───────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                    Epoll 事件循环 (主线程)                      │
│                                                              │
│   ┌──────────────────────────────────────────────────┐      │
│   │           drainResponseQueue()                    │      │
│   │    从 response_queue_ 取出响应, 编码后写入         │      │
│   │    对应连接的 output_buffer                       │      │
│   │    校验 conn_id 防止 fd 复用脏响应                  │      │
│   │    output_buffer 超限时关闭连接                    │      │
│   └──────────────────────┬───────────────────────────┘      │
│                          │                                   │
│                          ▼                                   │
│   ┌──────────────────────────────────────────────────┐      │
│   │           Connection::output_buffer               │      │
│   │    待发送数据缓冲区, handleWrite 中执行 send       │      │
│   └──────────────────────┬───────────────────────────┘      │
└──────────────────────────┼───────────────────────────────────┘
                           │
                           │  TCP 响应
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                       客户端 (Client)                         │
└──────────────────────────────────────────────────────────────┘
```

---

## 3. 核心模块

### 3.1 网络层 (`net/`)

| 文件 | 职责 |
|---|---|
| `TcpServer.hpp/cpp` | epoll 事件循环、连接管理、IO 处理、响应排空 |
| `Connection.hpp` | 连接对象，维护 fd、conn_id、输入/输出缓冲区 |
| `SocketUtil.hpp` | 非阻塞 socket 设置工具 |

### 3.2 协议层 (`protocol/`)

| 文件 | 职责 |
|---|---|
| `ProtocolCodec.hpp/cpp` | 二进制协议编码/解码，处理粘包和半包 |
| `MessageType.hpp` | 消息类型枚举定义 |
| `Request.hpp` | 请求数据结构 |
| `Response.hpp` | 响应数据结构 |

### 3.3 业务层 (`business/`)

| 文件 | 职责 |
|---|---|
| `Dispatcher.hpp/cpp` | 消息分发器，按 MessageType 路由到具体 Handler |
| `Handlers.hpp/cpp` | 业务处理函数（PING、ECHO、LOG_PUSH、STATS） |
| `StatsManager.hpp/cpp` | 全局监控统计（请求数、错误数、连接数、流量等） |
| `LogStorage.hpp/cpp` | 日志文件持久化存储 |

### 3.4 并发层 (`concurrent/`)

| 文件 | 职责 |
|---|---|
| `BlockQueue.hpp` | 线程安全阻塞队列，用于 IO 线程与 Worker 线程解耦 |

### 3.5 通用模块 (`common/`)

| 文件 | 职责 |
|---|---|
| `Logger.hpp` | 线程安全日志模块（可变参数模板） |

---

## 4. 请求完整链路

```
1. 客户端通过 TCP 连接发送二进制协议包
2. epoll_wait 触发 EPOLLIN 事件
3. handleRead() 读取数据到 Connection::input_buffer
4. ProtocolCodec::decode() 解析完整请求
5. 解码后的 Request 推入 request_queue_
6. Worker 线程从 request_queue_ pop 请求
7. Dispatcher::dispatch() 根据 MessageType 分发
8. 对应 Handler 处理业务逻辑，生成 Response
9. Response 推入 response_queue_
10. drainResponseQueue() 从 response_queue_ 取出响应
11. 校验 conn_id，编码为二进制包，写入 Connection::output_buffer
12. 启用 EPOLLOUT 事件
13. handleWrite() 执行 send 发送数据
```

---

## 5. 核心设计决策

### 5.1 为什么选择 epoll ET（边缘触发）模式

ET 模式下，epoll 只在状态变化时通知一次，配合非阻塞 socket 的 while 循环读取，可以在一次事件中尽可能多地处理数据。相比 LT 模式，ET 减少了 epoll 事件数量，但要求程序必须正确处理 EAGAIN。

### 5.2 为什么使用线程池解耦 IO 和业务

IO 线程如果同时执行业务逻辑（如 JSON 解析、文件写入），会阻塞事件循环，影响其他连接的响应速度。通过 BlockQueue 将业务处理委托给 Worker 线程池，IO 线程可以专注于网络事件处理。

### 5.3 为什么使用自定义二进制协议

相比 HTTP，自定义二进制协议解析成本低、传输效率高，适合长连接消息接入场景。协议采用长度前缀方式（TLV 风格的 Type-Length-Value 变体），天然支持粘包和半包处理。

### 5.4 为什么使用 output_buffer

异步 Worker 线程处理完请求后，不能直接发送响应，因为：
- Worker 线程不知道连接是否仍然有效
- 直接发送会阻塞 Worker 线程
- 批量发送效率更高

因此响应先存入 output_buffer，由主事件循环统一排空发送。

### 5.5 为什么引入 conn_id

fd 是操作系统分配的整数，会被回收复用。如果 Worker 处理完一个请求时对应的连接已经关闭，fd 被新连接复用，旧响应就可能发送给新连接。conn_id（连接世代号）确保响应只发给它对应的连接实例。

---

## 6. 关键数据流

```
请求流向:
  客户端 → [epoll IN] → input_buffer → decode() → Request → request_queue_
   → Worker → Dispatcher → Handler → Response → response_queue_
   → drainResponseQueue() → output_buffer → [epoll OUT] → send() → 客户端

错误响应:
  decode 失败 → closeConnection()
  Handler 返回 ERROR_RESP → 正常响应链路发送
  Worker 异常 → try-catch 捕获，日志记录
  output_buffer 超限 → closeConnection()
```

---

## 7. 线程模型

| 线程 | 数量 | 职责 |
|---|---|---|
| 主线程（事件循环） | 1 | epoll_wait、accept、read、write、drainResponseQueue |
| Worker 线程池 | min(hardware_concurrency, 4) | 从 request_queue_ 取请求，分发处理，推入 response_queue_ |

当前线程模型是单 Reactor 模型：一个主线程处理所有网络事件，Worker 线程池处理业务。在中等并发场景下够用，高并发时主线程可能成为瓶颈。
