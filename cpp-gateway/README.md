# MessageServer — C++17 Epoll TCP Server

一个基于 Linux `epoll`（ET 边缘触发模式）和非阻塞 socket 实现的轻量级 TCP 消息服务端，支持多消息类型、异步 Worker 线程池、日志落盘与实时监控统计。

---

## 📂 项目结构

```
cpp-epoll-tcp-server
├── include/
│   ├── common/
│   │   └── Logger.hpp              # 线程安全日志模块（可变参数模板）
│   ├── concurrent/
│   │   └── BlockQueue.hpp           # 阻塞/非阻塞队列，IO 与 Worker 解耦
│   ├── net/
│   │   ├── Connection.hpp           # 连接对象（含 conn_id 世代校验）
│   │   ├── SocketUtil.hpp           # Socket 工具函数（非阻塞设置）
│   │   └── TcpServer.hpp            # 核心服务器接口
│   ├── protocol/
│   │   ├── MessageType.hpp          # 消息类型枚举（PING/ECHO/LOG_PUSH/STATS…）
│   │   ├── ProtocolCodec.hpp        # 编解码器（定长头+变长体协议）
│   │   ├── Request.hpp              # 请求数据结构
│   │   └── Response.hpp             # 响应数据结构
│   ├── business/
│   │   ├── Dispatcher.hpp           # 消息分发器
│   │   ├── Handlers.hpp             # 业务处理函数声明
│   │   ├── LogStorage.hpp           # 日志落盘存储
│   │   └── StatsManager.hpp         # 全局监控统计
│   └── nlohmann/
│       └── json.hpp                 # JSON 解析库（单头文件）
├── src/
│   ├── main.cpp                     # 服务端入口
│   ├── net/
│   │   └── TcpServer.cpp            # epoll 事件循环（核心，~474 行）
│   ├── protocol/
│   │   └── ProtocolCodec.cpp        # 粘包/半包处理 & 编码解码
│   └── business/
│       ├── Dispatcher.cpp           # 请求分发
│       ├── Handlers.cpp             # 业务处理实现
│       ├── LogStorage.cpp           # 日志追加写入
│       └── StatsManager.cpp         # 监控指标管理
├── scripts/
│   ├── benchmark.py                 # 压测工具（并发连接 × 请求数分布）
│   ├── test_half_packet.py          # 半包（split-send）稳定性测试
│   ├── test_sticky_packet.py        # 粘包（多包合一 send）测试
│   ├── test_invalid_length.py       # 恶意超长 body_length 熔断测试
│   └── test_slow_client.py          # 慢客户端输出缓冲区背压测试
├── results/                         # 压测结果 JSON（含延迟分布统计）
│   ├── echo_1x1000.json
│   ├── echo_10x1000.json
│   ├── echo_100x100.json
│   ├── echo_300x50.json
│   ├── echo_500x20.json
│   ├── log_push_100x100.json
│   └── run_summary.txt
├── docs/                            # 详细设计文档
│   ├── architecture.md              # 架构设计
│   ├── protocol.md                  # 协议细节
│   ├── connection_lifecycle.md      # 连接生命周期
│   ├── error_handling.md            # 错误处理策略
│   ├── performance.md               # 性能分析与调优
│   ├── roadmap.md                   # 开发路线图
│   └── ai_command.md                # AI 协作指令
├── tests/                           # 单元测试（开发中）
├── logs/
│   └── access.log                   # 业务日志文件
├── CMakeLists.txt
└── README.md
```

---

## 🛠️ 技术栈

| 层级 | 技术 |
|---|---|
| 语言 | **C++17** |
| 网络 IO | **Linux epoll**（Edge Triggered）+ 非阻塞 socket |
| 并发模型 | **多线程**：`std::thread` + `std::mutex` + `std::condition_variable` |
| 协议 | **自定义长度前缀协议**（4字节网络序长度 + body） |
| 构建 | **CMake** + g++ |
| JSON | **nlohmann/json**（单头文件） |
| 测试/压测 | **Python 3**（socket + struct 发包） |

---

## ✨ 核心特性

### 网络层
- **epoll ET 事件循环**：采用边缘触发模式，while 循环榨干缓冲区直到 `EAGAIN`。
- **非阻塞 accept**：一次性处理完所有待 accept 的连接。
- **IO 与 Worker 解耦**：IO 线程只负责 `epoll_wait` + `recv` + 解码，将完整 `Request` 入队；Worker 线程从队列取任务执行业务，避免耗时操作阻塞网络循环。
- **容量自适应的 Worker 线程池**：根据 CPU 核心数动态决定 Worker 数量（上限 4），队列空时休眠，stop 时优雅退出。
- **双队列解耦**：IO → RequestQueue → Worker → ResponseQueue → IO，形成完整流水线。

### 协议层
- **长度前缀协议**：`| 4 字节网络序 body_length | 1 字节 version | 1 字节 type | 8 字节 request_id | 变长 payload |`
- **粘包/半包处理**：解码器逐字节游标解析，剩余数据回退到 `input_buffer` 等待下次事件。
- **`NEED_MORE_DATA` 语义**：精确区分「数据不够」和「解析完成」，避免半包误处理。
- **协议边界校验**：校验最小固定头（10 字节）和最大 body（`FIXED_BODY_SIZE + MAX_PAYLOAD_SIZE`），恶意超长包直接拒绝并断开连接。

### 业务层
- **PING / PONG**：基础连通性探测。
- **ECHO**：回显测试。
- **LOG_PUSH / LOG_ACK**：JSON 格式日志上报、校验与落盘。
- **STATS / STATS_RESP**：实时服务器监控统计（请求数、错误数、流量、队列积压等）。

### 可靠性打磨
- **`conn_id` 世代校验**：每个连接分配独立递增 ID，响应发送前校验 `conn_id` 是否匹配，根除 fd 复用导致的跨连接数据串台。
- **Worker 异常隔离**：双层 `try-catch` 防护，业务异常不会击穿线程边界导致进程退出。
- **SIGPIPE 防护**：进程级 `signal(SIGPIPE, SIG_IGN)` + 每路 send 使用 `MSG_NOSIGNAL`。
- **输出缓冲区背压**：单连接 `output_buffer` 上限 2MB，超限直接断开慢客户端防止 OOM。
- **幂等关闭**：`closeConnection()` 先确认 fd 存在再操作，`decrementConnections()` CAS 下溢保护。
- **优雅停机**：`stop()` 先收集 fd 再逐个关闭，确保 `epfd` 在 `closeConnection` 之后才关闭。
- **epoll 错误处理**：`EPOLL_CTL_ADD` 失败仅关闭 client fd 不 exit 进程；`modifyConnectionEvents()` 统一封装返回值检查。
- **JSON 字段强校验**：使用 nlohmann/json 解析器代替字符串 find，确保 key 存在且类型正确。

---

## 🚀 快速开始

### 前置条件
- Linux 环境（WSL2 亦可）
- g++ 支持 C++17
- CMake >= 3.10
- Python 3（运行测试脚本）

### 编译

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### 运行服务端

```bash
cd build
./message_server
# 服务端默认监听 9000 端口，Ctrl+C 触发优雅关闭
# 可用 GATEWAY_ID、GATEWAY_PORT、CONTROL_PLANE_HOST、CONTROL_PLANE_PORT 覆盖运行配置
```

### 运行测试

```bash
# 确保服务端已在运行，新开终端进入 scripts/ 目录

# 1. 粘包测试 — 一个 sendall() 发送多个完整包
python3 test_sticky_packet.py

# 2. 半包测试 — 分两次发送，中间间隔 1.5s
python3 test_half_packet.py

# 3. 恶意长度测试 — 伪造超长 body_length（OOM 熔断）
python3 test_invalid_length.py

# 4. 慢客户端测试 — 不读取响应，触发输出缓冲区背压
python3 test_slow_client.py
```

### 压测

```bash
# 100 个并发连接各发 100 条 ECHO 消息
python3 benchmark.py --clients 100 --requests 100 --type echo

# 1 个连接发 1000 条消息，保存结果
python3 benchmark.py --clients 1 --requests 1000 --type echo --output results/echo_1x1000.json

# LOG_PUSH 压测
python3 benchmark.py --clients 100 --requests 100 --type log_push
```

---

## 📊 消息协议

### 请求格式（Request）

```
| 4 字节 body_length (网络序) | 1 字节 version | 1 字节 type | 8 字节 request_id (网络序) | N 字节 payload |
```

`body_length` = 10 (固定头) + payload 长度，最大不超过 `MAX_PAYLOAD_SIZE + 10`（约 4 MB）。

### 支持的 MessageType

| Type | 请求 | 响应 | 说明 |
|---|---|---|---|
| 1 → 5 | `PING` | `PONG` | 连通性探测 |
| 2 → 6 | `ECHO` | `ECHO_RESP` | 回显（原样返回 payload） |
| 3 → 8 | `LOG_PUSH` | `LOG_ACK` | JSON 日志上报与落盘 |
| 4 → 9 | `STATS` | `STATS_RESP` | 实时服务器监控统计 |
| 其他 → 7 | — | `ERROR_RESP` | 未知消息类型兜底 |

### 错误响应格式

```json
{"status": 400, "message": "具体的错误描述"}
```

### LOG_PUSH 要求的 JSON 格式

```json
{
  "level": "INFO",
  "service": "auth-service",
  "message": "user login success"
}
```

三个字段均为必填且必须为字符串类型，否则返回 `400 invalid log format`。

---

## 📈 监控统计

请求 `STATS`（type=4）可获得 JSON 格式的实时服务器状态：

```json
{
  "total_requests": 1024,
  "total_logs": 512,
  "total_errors": 0,
  "total_recv_bytes": 65536,
  "total_sent_bytes": 65536,
  "active_connections": 5,
  "total_request_queue_backlog": 0,
  "total_response_queue_backlog": 0
}
```

---

## 📊 基准测试（Benchmark）

项目提供了 `benchmark.py` 脚本，支持多类型消息压测及延迟分布统计。典型结果存放于 `results/` 目录。

### 测试场景覆盖

| 场景 | 脚本 | 验证点 |
|---|---|---|
| 粘包 | `test_sticky_packet.py` | 一次 send 多发，全部正确解码 |
| 半包 | `test_half_packet.py` | 分片到达，等待完整后再处理 |
| 非法长度 | `test_invalid_length.py` | OOM 防护，断开 + 不崩溃 |
| 慢客户端 | `test_slow_client.py` | 输出背压断连，服务不受影响 |
| 高并发 | `benchmark.py` | QPS、延迟分布（P50/P95/P99） |

---

## 📖 文档

详细设计文档位于 `docs/` 目录：

- **[架构设计](docs/architecture.md)** — 整体架构、线程模型、数据流
- **[协议细节](docs/protocol.md)** — 二进制协议格式、编解码流程
- **[连接生命周期](docs/connection_lifecycle.md)** — 从 accept 到 close 的全流程
- **[错误处理](docs/error_handling.md)** — 异常分类、防护策略、恢复机制
- **[性能分析](docs/performance.md)** — 压测结果、瓶颈分析、调优建议
- **[开发路线图](docs/roadmap.md)** — 规划中的特性与改进

---

## 🔧 构建配置

`CMakeLists.txt` 编译目标：`message_server`

| 选项 | 值 |
|---|---|
| C++ 标准 | C++17 |
| 编译选项 | `-Wall -Wextra -O2` |
| 外部依赖 | `Threads::Threads`（系统多线程库） |

---

## 📝 License

MIT
