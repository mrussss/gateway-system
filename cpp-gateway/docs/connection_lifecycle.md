# 连接生命周期与资源管理

## 1. 连接生命周期总览

```
                        accept()
                           │
                   设置非阻塞模式
                           │
                   epoll_ctl ADD
                   (EPOLLIN | EPOLLET)
                           │
                   分配 conn_id
                           │
                connections_.emplace()
                StatsManager::incrementConnections()
                           │
                           ▼
              ┌──────────────────────────┐
              │      连接活跃阶段           │
              │                          │
              │  read / decode / dispatch │
              │  worker → response_queue  │
              │  drain → output_buffer    │
              │  write → send             │
              │  EPOLLIN / EPOLLOUT 切换   │
              └──────────┬───────────────┘
                         │
                 关闭触发条件
                         │
           ┌─────────────┼─────────────┐
           ▼             ▼             ▼
      对端关闭 EOF    recv 错误    output_buffer
      (bytes_read=0)  (errno!=    超限（慢客户端
                      EAGAIN)     保护）
           └─────────────┼─────────────┘
                         ▼
               closeConnection(fd)
                         │
               ┌─────────┴─────────┐
               │  清理流程:          │
               │  1. 从 connections_ │
               │     查找 fd        │
               │  2. epoll_ctl DEL  │
               │  3. close(fd)      │
               │  4. connections_    │
               │     erase          │
               │  5. decrement       │
               │     Connections()   │
               └───────────────────┘
```

---

## 2. 连接创建

### 2.1 accept 新连接

连接在 `handleAccept()` 中创建：

```cpp
// 非阻塞 accept 循环
while (true) {
    fd = accept(listen_fd_, ...);
    if (fd == -1) {
        if (errno == EAGAIN) break;  // 没有更多新连接
        else break;                  // 真正的错误
    }
    setNonBlocking(fd);

    // 挂载到 epoll
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event) == -1) {
        close(fd);       // 挂载失败只关闭这个 fd，不退出进程
        continue;
    }

    // 分配连接 ID
    uint64_t conn_id = next_conn_id_.fetch_add(1);
    connections_.emplace(fd, Connection(fd, conn_id));
    StatsManager::getInstance().incrementConnections();
}
```

关键设计点：

- **accept4 替代 accept + setNonBlocking**: 当前使用 `accept()` + `setNonBlocking()`。可优化为 `accept4()` 一次调用完成。
- **epoll_ctl 失败不退出进程**: 单个客户端挂载失败不影响服务器整体运行。
- **conn_id 递增分配**: 每个连接获取全局唯一世代号，用于后续 fd 复用防护。

### 2.2 Connection 对象

```cpp
struct Connection {
    int fd;                  // 文件描述符
    uint64_t conn_id;        // 连接世代号（递增）
    std::string input_buffer;   // 接收缓冲区
    std::string output_buffer;  // 发送缓冲区
    bool closing = false;       // 关闭标记
};
```

每个连接独立维护 input_buffer 和 output_buffer，IO 线程从 socket 读到 input_buffer，解码后通过队列交给 Worker 处理；Worker 生成的响应先写入 output_buffer，再由事件循环统一发送。

---

## 3. 数据读取

### 3.1 handleRead 流程

```cpp
handleRead(fd):
  1. 从 connections_ 查找 fd，不存在则返回
  2. while 循环 recv 数据：
     a. bytes_read > 0  → 追加到 conn.input_buffer，继续读
     b. bytes_read == 0  → 对端关闭，closeConnection，退出循环
     c. bytes_read == -1 && errno == EAGAIN → decodeAndEnqueue，退出循环
     d. bytes_read == -1 && 其他错误 → closeConnection，退出循环
  end
```

### 3.2 decodeAndEnqueue

```cpp
bool decodeAndEnqueue(Connection &conn):
  1. 调用 ProtocolCodec::decode(conn.input_buffer, conn.fd, out_requests, conn.conn_id)
  2. 如果返回 INVALID_LENGTH → closeConnection，return false
  3. 将解码出的所有 Request 推入 request_queue_
  4. return true
```

设计要点：
- decode 可能从 input_buffer 中解析出多个完整请求（粘包场景），全部推入队列
- 解码失败时直接关闭连接，不残留有害数据
- decode 函数更新 input_buffer（erase 已处理的数据），保持缓冲区整洁

---

## 4. 数据发送

### 4.1 响应回写流程

```cpp
drainResponseQueue():
  while Try_pop(resp):
    1. connections_.find(resp.fd) → 连接已关闭则跳过
    2. conn.conn_id != resp.conn_id → 过期响应，丢弃
    3. 编码 resp 为二进制包
    4. 检查 output_buffer + encoded 是否超过 MAX_OUT_BUFFER_SIZE
       → 超限则 closeConnection，跳过
    5. encoded 追加到 conn.output_buffer
    6. modifyConnectionEvents(fd, EPOLLIN | EPOLLOUT | EPOLLET)

handleWrite(fd):
  while conn.output_buffer 不为空:
    1. send(fd, ...)
    2. sent_bytes > 0  → erase 已发送数据
    3. sent_bytes == -1 && EAGAIN → 下次再发，break
    4. sent_bytes == -1 && 其他错误 → closeConnection
    5. output_buffer 已空 → modifyConnectionEvents(fd, EPOLLIN | EPOLLET)
```

### 4.2 conn_id 校验

`drainResponseQueue()` 发送前检查 conn_id：

```cpp
if (conn.conn_id != resp.conn_id) {
    // 该 fd 已被回收复用，旧响应属于之前的连接
    // 丢弃此响应
    continue;
}
```

这个检查防止以下场景的脏响应：

```
1. 连接 A (fd=7, conn_id=1) 发送请求
2. Worker 开始处理 A 的请求
3. 连接 A 断开，fd=7 被回收
4. 新连接 B (fd=7, conn_id=2) 接入
5. Worker 完成 A 的请求，生成 Response(conn_id=1)
6. drain 时检查 conn.conn_id(2) != resp.conn_id(1) → 丢弃
```

---

## 5. 连接关闭

### 5.1 统一关闭入口

所有连接关闭都必须通过 `closeConnection(fd)`，不允许直接 `close(fd)`。

```cpp
void closeConnection(int fd) {
    // 幂等检查：fd 已不存在则直接返回
    auto it = connections_.find(fd);
    if (it == connections_.end()) return;

    // 从 epoll 移除（epfd 可能已被 stop 关闭）
    if (epfd_ != -1) {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    // 关闭 socket
    close(fd);

    // 清理连接状态
    connections_.erase(it);

    // 更新统计
    StatsManager::getInstance().decrementConnections();
}
```

### 5.2 必须统一入口的原因

| 原因 | 说明 |
|---|---|
| 幂等安全 | 重复调用不会 double-free 或重复减计数 |
| epoll 清理 | 关闭前必须从 epoll 实例移除 |
| 统计一致 | `decrementConnections()` 只能调用一次 |
| fd 泄漏防护 | 擦除 `connections_` 条目，防止后续通过 fd 操作已关闭的连接 |

### 5.3 关闭触发条件

| 场景 | 检测方式 |
|---|---|
| 对端正常关闭 | `recv` 返回 0（EOF） |
| 对端异常断开 | `recv` 返回 -1，errno 不是 EAGAIN |
| 协议校验失败 | `decode` 返回 `INVALID_LENGTH` |
| 输出缓冲区超限 | `output_buffer` 超过 2MB |
| 服务停止 | `stop()` 遍历所有连接逐个关闭 |
| epoll 操作失败 | `modifyConnectionEvents` 失败时关闭连接 |

### 5.4 优雅停机

```cpp
void stop() {
    // 1. 标记停止，防止重复执行
    is_stopped_ = true;
    running_ = false;

    // 2. 关闭监听 fd
    close(listen_fd_);

    // 3. 收集 fd，逐个关闭（防止遍历时 map 被修改）
    std::vector<int> fds;
    for (const auto &[fd, conn] : connections_) {
        fds.push_back(fd);
    }
    for (int fd : fds) {
        closeConnection(fd);
    }

    // 4. 停止队列
    request_queue_.stop();
    response_queue_.stop();

    // 5. 等待 Worker 线程退出
    for (auto &worker : workers_) {
        if (worker.joinable()) worker.join();
    }

    // 6. 关闭 epoll fd（必须在 closeConnection 之后）
    if (epfd_ != -1) {
        close(epfd_);
    }
}
```

关闭顺序的核心原则：
- **epfd_ 最后关**：因为 `closeConnection()` 中需要 `epoll_ctl(epfd_, EPOLL_CTL_DEL)`
- **队列先 stop 再 join**：确保 Worker 能从 pop 返回并正常退出
- **先收集 fd 再遍历关闭**：防止遍历过程中 map 被修改

---

## 6. 连接计数管理

`StatsManager` 使用原子操作保护连接计数，`decrementConnections()` 包含下溢保护：

```cpp
void decrementConnections() {
    uint64_t current = active_connections.load();
    while (current > 0) {
        if (active_connections.compare_exchange_weak(current, current - 1)) {
            return;
        }
    }
    // current == 0 时不再减少
}
```

CAS（Compare-And-Swap）循环确保并发安全，同时防止计数下溢到 `UINT64_MAX`。

---

## 7. EPOLLIN / EPOLLOUT 切换

通过封装的 `modifyConnectionEvents()` 管理 fd 的 epoll 事件：

```cpp
bool modifyConnectionEvents(int fd, uint32_t events) {
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.fd = fd;

    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        // 操作失败时关闭连接
        closeConnection(fd);
        return false;
    }
    return true;
}
```

事件切换规则：

| 场景 | 新事件 |
|---|---|
| 有新数据要发送 | `EPOLLIN \| EPOLLOUT \| EPOLLET` |
| output_buffer 已空 | `EPOLLIN \| EPOLLET` |

注意：EPOLLOUT 是可写事件，不需要一直开启。只在有数据要发送时才注册 EPOLLOUT，数据发送完毕立即移除，避免 epoll_wait 频繁返回 EPOLLOUT 导致 busy-loop。
