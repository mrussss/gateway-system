# 异常处理与边界防护

## 1. 总览

MessageServer 在多个层次上对异常输入和边界情况进行防护：

| 防护层次 | 防护内容 |
|---|---|
| 协议层 | 非法包长、半包、版本校验 |
| 业务层 | JSON 格式与字段校验、未知消息类型 |
| 网络层 | 输出缓冲区背压、epoll 操作失败、SIGPIPE |
| 线程层 | Worker 异常隔离、线程安全队列 |

每一层的设计原则：**异常输入不会导致进程退出，错误连接被关闭后不影响其他连接**。

---

## 2. 协议层防护

### 2.1 非法 body_length

解码器在解析任何数据之前先校验包长：

```cpp
if (host_body_length < FIXED_BODY_SIZE || host_body_length > MAX_BODY_SIZE) {
    return DecodeStatus::INVALID_LENGTH;  // 调用方据此关闭连接
}
```

校验覆盖的场景：

| 输入 | 行为 |
|---|---|
| `body_length = 0` | 判定非法，关闭连接 |
| `body_length = 5`（小于 FIXED_BODY_SIZE=10） | 判定非法，关闭连接 |
| `body_length = 999999999`（超过 MAX_BODY_SIZE） | 判定非法，关闭连接 |
| `body_length = 4MB+1`（超过 MAX_PAYLOAD_SIZE+FIXED_BODY_SIZE） | 判定非法，关闭连接 |

这避免了无符号整数下溢（`host_body_length - FIXED_BODY_SIZE`）和恶意超大包导致的 OOM。

### 2.2 半包（数据分片到达）

TCP 是流式协议，一个完整的应用层包可能被拆分为多次 recv 到达。解码器通过游标 `read_index` 和 `need_more_data` 标记处理半包：

```
1. 检查剩余数据是否 ≥ 4 字节（读取 body_length 的最低要求）
   不满足 → need_more_data = true，跳出循环
2. 检查剩余数据是否 ≥ 4 + body_length
   不满足 → need_more_data = true，跳出循环
3. 解析完整包，游标前进
4. 检查是否还有更多完整包，重复步骤 1-3
5. 清理已处理数据，返回 OK 或 NEED_MORE_DATA
```

调用方根据返回值决定是否等待更多数据：

```cpp
// TcpServer::decodeAndEnqueue
DecodeStatus status = ProtocolCodec::decode(...);
if (status == DecodeStatus::INVALID_LENGTH) {
    closeConnection(conn.fd);  // 非法数据，关闭连接
    return false;
}
// OK 或 NEED_MORE_DATA 都继续等待数据
```

### 2.3 粘包（多条请求同时到达）

一次 recv 可能包含多条完整请求。解码器通过 while 循环连续解析，每次解析后游标前移一个包的长度，直到剩余数据不足以构成一个完整包。

例如：

```
一次 recv 数据: | 包1 | 包2 | 包3 的前半部分 |
输出: out_requests = [包1, 包2], 返回 NEED_MORE_DATA
```

所有解析出的完整请求都会被推入 `request_queue_`，不会因为后续数据不完整而丢弃已解析的请求。

---

## 3. 业务层防护

### 3.1 JSON 格式校验

LOG_PUSH 的 payload 使用 `nlohmann/json` 严格解析：

```cpp
try {
    auto j = nlohmann::json::parse(request.payload);

    if (!j.is_object() ||
        !j.contains("level") || !j["level"].is_string() ||
        !j.contains("service") || !j["service"].is_string() ||
        !j.contains("message") || !j["message"].is_string())
    {
        return makeErrorResponse(request, 400, "invalid log format");
    }
}
catch (const nlohmann::json::parse_error &) {
    return makeErrorResponse(request, 400, "invalid json");
}
```

这避免了使用 `payload.find("level")` 这种字符串搜索导致的误判（例如 payload 中包含 `"elevated":"true"` 时，`find("level")` 会错误匹配）。

### 3.2 空 payload / 超大 payload

```cpp
if (request.payload.empty()) {
    return makeErrorResponse(request, 400, "payload is empty");
}
if (request.payload.size() > 4096) {
    return makeErrorResponse(request, 400, "payload too large");
}
```

LOG_PUSH 和其他业务 Handler 可对 payload 长度做独立限制（这里的 4096 是业务层限制，区别于协议层的 4MB MAX_PAYLOAD_SIZE）。

### 3.3 未知消息类型

Dispatcher 的 default 分支返回错误响应，而不是崩溃或忽略：

```cpp
switch (request.type) {
    case MessageType::PING:      return handlePing(request);
    case MessageType::ECHO:      return handleEcho(request);
    case MessageType::LOG_PUSH:  return handleLogPush(request);
    case MessageType::STATS:     return handleStats(request);
    default:
        return makeErrorResponse(request);  // 返回 {"status":400,"message":"unknown type"}
}
```

### 3.4 错误响应格式

所有错误响应使用统一格式：

```cpp
Response makeErrorResponse(const Request &request, int status, const std::string &message) {
    // 构造 {"status":400,"message":"具体的错误描述"}
    nlohmann::json j;
    j["status"] = status;
    j["message"] = message;
    resp.payload = j.dump();
    return resp;
}
```

---

## 4. 网络层防护

### 4.1 输出缓冲区上限（慢客户端保护）

如果客户端接收缓慢或不读取响应，output_buffer 会持续增长。服务端设置 2MB 上限：

```cpp
constexpr size_t MAX_OUT_BUFFER_SIZE = 2 * 1024 * 1024;  // 2MB

// drainResponseQueue 中追加数据前检查
if (conn.output_buffer.size() + encoded_data.size() > MAX_OUT_BUFFER_SIZE) {
    // 已超过上限，关闭连接
    closeConnection(conn.fd);
    continue;
}
```

这保证单个慢客户端不会耗尽服务器内存。

### 4.2 SIGPIPE 防护

Linux 下向已关闭的连接 send 会触发 SIGPIPE 信号，默认行为是终止进程。服务端从两个层面防护：

**进程级**：启动时忽略 SIGPIPE

```cpp
std::signal(SIGPIPE, SIG_IGN);
```

**调用级**：send 时使用 MSG_NOSIGNAL

```cpp
ssize_t sent_bytes = send(fd, data, size, MSG_NOSIGNAL);
```

双重防护确保 send 到已关闭连接时只返回错误，不会杀死进程。

### 4.3 epoll 操作失败

**EPOLL_CTL_ADD 失败**：accept 新连接时如果挂载 epoll 失败，只关闭该 client fd：

```cpp
if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event) == -1) {
    close(fd);   // 只关闭这个客户端
    continue;    // 继续处理下一个连接
}
```

错误设计对比：
- ❌ 旧代码：`close(epfd_); exit(EXIT_FAILURE);` — 一个客户端失败退出整个服务器
- ✅ 新代码：`close(fd); continue;` — 仅关闭异常连接，不影响服务器运行

**EPOLL_CTL_MOD 失败**：通过统一的 `modifyConnectionEvents()` 处理：

```cpp
bool modifyConnectionEvents(int fd, uint32_t events) {
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) == -1) {
        closeConnection(fd);  // 操作失败时关闭连接
        return false;
    }
    return true;
}
```

### 4.4 recv 错误处理

```cpp
if (bytes_read > 0) {
    // 正常收到数据，追加到 input_buffer
}
else if (bytes_read == 0) {
    // 对端关闭连接，清理资源
    closeConnection(fd);
    break;
}
else if (bytes_read == -1) {
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        // 数据读取完毕，进入解码流程
        decodeAndEnqueue(conn);
        break;
    } else {
        // 真正的 recv 错误
        closeConnection(fd);
        break;
    }
}
```

关键点：
- `bytes_read == 0` 时**不再尝试解码 input_buffer 中的残留数据**。因为对端关闭后即使解码出请求，worker 处理完也无法可靠回包
- EAGAIN 路径才调用 `decodeAndEnqueue`

### 4.5 连接关闭幂等性

```cpp
void closeConnection(int fd) {
    auto it = connections_.find(fd);
    if (it == connections_.end()) {
        return;  // fd 不存在，说明已关闭，忽略
    }
    // ... 关闭逻辑
}
```

如果某个 fd 已被关闭，再次调用 `closeConnection` 会直接返回，不会重复关闭或错误操作。

---

## 5. 线程层防护

### 5.1 Worker 异常隔离

Worker 线程通过双层 try-catch 防止异常逃逸：

```cpp
try  // 外层：线程入口
{
    while (true) {
        Request req;
        bool ok = request_queue_.pop(req);
        if (!ok) break;  // 队列已停止

        try  // 内层：单个请求处理
        {
            Response resp = dispatch.dispatch(req);
            response_queue_.push(resp);
        }
        catch (const std::exception &e) {
            // 业务层异常：记录日志，继续处理下一个请求
            LOG_INFO("Worker exception: %s", e.what());
        }
        catch (...) {
            // 未知异常
            LOG_INFO("Worker unknown exception");
        }
    }
}
catch (const std::exception &e) {
    LOG_INFO("Worker fatal: %s", e.what());
}
```

内层 catch 处理业务异常（dispatch 或 push 失败），外层 catch 兜底线程级的致命错误。任何异常都不会导致 `std::terminate()`。

### 5.2 线程安全队列

`BlockQueue` 使用 `std::mutex` 和 `std::condition_variable` 实现：

- **pop()**: 队列空时阻塞等待，队列停止且空时返回 false
- **Try_pop()**: 队列空时立即返回 false（不检查 stop 状态，确保停止前已入队的数据能被消费）
- **push()**: 队列停止时返回 false，不再接受新数据
- **stop()**: 唤醒所有阻塞的 pop 调用

### 5.3 索引计数异常

`decrementConnections()` 使用 CAS 循环防止下溢：

```cpp
void decrementConnections() {
    uint64_t current = active_connections.load();
    while (current > 0) {
        if (active_connections.compare_exchange_weak(current, current - 1)) {
            return;
        }
    }
    // 当前值为 0 时不再减少
}
```

---

## 6. 资源与统计一致性

| 场景 | 防护措施 |
|---|---|
| 连接计数为 0 时仍调用 decrement | CAS 下溢保护，不会减到 UINT64_MAX |
| 重复关闭同一 fd | closeConnection 幂等，先检查 fd 是否存在 |
| stop 中途新增连接 | 先收集 fd 列表再遍历关闭 |
| epfd 提前关闭 | closeConnection 中检查 epfd != -1 |
| 响应队列已经 stop | Try_pop 返回值指导上层行为 |
