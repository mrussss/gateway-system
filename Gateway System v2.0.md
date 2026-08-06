# Gateway System v2.0
# ControlPlaneClient 与独立 AUTH Executor 完整重构开发文档

> 适用仓库：`mrussss/gateway-system`
> 所属阶段：Phase 5 Completion
> 重构范围：C++ 同步 HTTP Client、独立有界 AUTH Executor、Go/Redis AUTH 安全限速
> 最终定位：严格、有界、可观测、可测试的内部认证链路
> 文档状态：最终实施方案

---

# 1. 重构背景

当前 Gateway 已经具备：

- C++17 非阻塞 TCP 数据面；
- 单 Reactor；
- `epoll` ET；
- `eventfd`；
- 有界 Request/Response Queue；
- Worker Pool；
- `fd + conn_id`；
- 动态配置快照；
- 慢客户端保护；
- 有截止时间的优雅退出；
- Go 控制面；
- Redis Token、配置和 Gateway 状态。

现有架构的核心原则是：

```text
Reactor
只管理 Socket、epoll、连接表和连接生命周期

Worker
只处理 Request，并生成 Response

Worker
→ Response Queue
→ eventfd
→ Reactor
```

该原则应继续保留，不重构现有 C++ 网络核心。

当前 AUTH 链路为：

```text
TCP Client
→ Reactor
→ 公共 Request Queue
→ 公共 Worker
→ Dispatcher
→ handleAuth()
→ 同步 ControlPlaneClient
→ Go /auth/check
→ Redis
```

现有公共 Worker 通过统一 Dispatcher 同时处理 AUTH、PING、ECHO、LOG_PUSH 和 STATS。

因此，当 Go 控制面或 Redis 延迟升高时：

```text
全部公共 Worker
可能同时卡在同步 AUTH HTTP 调用
```

随后普通业务请求只能继续在公共 Request Queue 中等待，最终可能发生：

```text
Worker 全部占用
→ Request Queue 积压
→ Request Queue 满
→ 普通请求被拒绝
```

此外，现有 `ControlPlaneClient` 还存在以下边界：

- 每次请求重新 `getaddrinfo()`；
- 每次新建 TCP 连接；
- 使用 `select()` 等待连接完成；
- connect 后恢复阻塞 Socket；
- 分别设置 `SO_SNDTIMEO` 和 `SO_RCVTIMEO`；
- 没有共享绝对 deadline；
- 读取响应直到 EOF；
- 不解析 `Content-Length`；
- 不区分非法凭证和控制面故障；
- `timeout_ms_` 硬编码为 1000ms。

当前实现确实是：

```text
非阻塞 connect + select
→ 恢复阻塞模式
→ SO_SNDTIMEO / SO_RCVTIMEO
→ send
→ recv 到 EOF
```



因此需要进行一次范围受控的重构。

---

# 2. 最终方案结论

最终采用：

```text
严格同步 HTTP Client
+
独立有界 AUTH Executor
+
Go/Redis AUTH 失败安全限速
```

具体包括：

```text
1. 小型严格同步 ControlPlaneClient
2. 独立 Auth Queue
3. 固定数量 Auth Worker
4. 普通 Worker 与 Auth Worker 执行隔离
5. connect/send/receive 共享绝对 deadline
6. poll() 替代 select()
7. 正确处理 Content-Length
8. 明确拒绝不支持的 HTTP 特性
9. AuthOutcome 与 HttpError 分层
10. Redis AUTH 失败 TTL 限速
11. 完整 shutdown、故障和恢复测试
```

不采用：

```text
io_uring
异步 HTTP Client
HTTP Reactor
连接池
Keep-Alive
本地认证缓存
HTTP/2
TLS
完整 chunked 解码
通用 HTTP Client 框架
双 Response Queue
多 Reactor
```

最终目标不是追求极限 AUTH 吞吐量，而是：

> 保证 Reactor 永不等待控制面；
> 保证 AUTH 故障不会占满普通 Worker；
> 保证所有等待和队列都有明确边界；
> 保证认证失败与基础设施故障语义可区分。

---

# 3. 最终架构

```text
TCP Client
    │
    │ 自定义长度前缀协议
    ▼
C++ Gateway Reactor
    ├── accept / recv / decode
    ├── connection ownership
    ├── fd + conn_id
    ├── output buffer
    ├── send / close
    └── epoll lifecycle
          │
          ├── AUTH
          │     ▼
          │  Bounded Auth Queue
          │     ▼
          │  Auth Worker Pool
          │     ▼
          │  Strict Sync ControlPlaneClient
          │     ▼
          │  Go POST /auth/check
          │     ▼
          │  Redis Token Hash / Failure Limiter
          │
          └── PING / ECHO / LOG_PUSH / STATS
                ▼
             Bounded Request Queue
                ▼
             Normal Worker Pool

Normal Worker + Auth Worker
          │
          ▼
Shared Bounded Response Queue
          │
          ▼
eventfd
          │
          ▼
Reactor
          │
          ▼
TCP Client
```

该方案实现的是：

```text
请求执行层 Bulkhead
```

即认证故障被隔离在 AUTH 子系统中。

它不是端到端完全隔离，因为两类 Worker 仍然共用：

```text
Response Queue
eventfd
Reactor
```

考虑 Auth Worker 数量较少、AUTH 响应体很小，当前不拆分双 Response Queue。

---

# 4. 核心设计原则

## 4.1 Reactor 所有权不变

Reactor 继续独占：

- `connections_`；
- Socket；
- epoll；
- Connection 状态；
- `authenticated`；
- `auth_pending`；
- `closing`；
- output buffer；
- fd 关闭；
- Response 应用。

Auth Worker 不得：

- 调用 `recv()` 或 `send()` 操作客户端 Socket；
- 修改 epoll；
- 修改 Connection；
- 直接标记连接已认证；
- 直接关闭客户端连接；
- 直接读取或遍历 `connections_`。

Auth Worker 只允许：

```text
读取 AuthTask 值
→ 调用 Go 控制面
→ 生成 Response 值
→ 放入 Response Queue
```

---

## 4.2 所有队列必须有界

最终队列：

```text
request_queue_
auth_queue_
response_queue_
```

全部具有固定容量。

队列满时：

```text
不阻塞 Reactor
不无限等待
不动态扩容
不静默丢弃
```

必须产生明确过载行为和指标。

---

## 4.3 Worker 数量本身就是并发上限

不再维护参与准入的 AUTH semaphore 或许可计数器。

```text
AUTH_WORKER_COUNT
= 同时执行的 AUTH 最大数量

AUTH_QUEUE_CAPACITY
= 等待执行的 AUTH 最大数量
```

可保留：

```cpp
std::atomic<size_t> auth_in_flight_{0};
```

但它只用于指标，不参与准入控制。

---

## 4.4 Fail Closed 不变

以下任何情况都不得认证成功：

- DNS 失败；
- TCP 连接失败；
- deadline 超时；
- HTTP 发送失败；
- HTTP 接收失败；
- HTTP 格式非法；
- 非 2xx 状态；
- JSON 非法；
- `allowed` 字段不存在；
- `allowed=false`；
- Redis 不可用；
- Go 控制面不可用。

但是必须区分：

```text
Denied
= 已确认凭证不合法

Unavailable
= 控制面、Redis、网络或协议异常
```

两者最终都不能认证成功，但指标、安全限速和日志语义不同。

---

# 5. 配置项

新增启动环境变量：

```text
CONTROL_PLANE_TIMEOUT_MS
AUTH_WORKER_COUNT
AUTH_QUEUE_CAPACITY
```

推荐演示默认值：

```text
CONTROL_PLANE_TIMEOUT_MS=1000
AUTH_WORKER_COUNT=2
AUTH_QUEUE_CAPACITY=32
```

这些只是项目默认值，不宣称为通用生产参数。

建议验证范围：

```text
CONTROL_PLANE_TIMEOUT_MS
最小：100
最大：30000

AUTH_WORKER_COUNT
最小：1
最大：16

AUTH_QUEUE_CAPACITY
最小：1
最大：65536
```

行为规则：

```text
未设置
→ 使用默认值

设置但格式非法
→ 启动失败

设置为越界值
→ 启动失败

不允许静默使用错误配置
```

环境变量统一在启动配置层读取：

```text
main()
→ parseStartupConfig()
→ validateStartupConfig()
→ TcpServer constructor
→ ControlPlaneClient constructor
```

`ControlPlaneClient` 不直接读取环境变量。

---

# 6. 内部 HTTP 协议契约

## 6.1 固定请求模型

仅支持：

```text
HTTP/1.1
GET
POST
JSON
Content-Length
Connection: close
```

请求路径来自代码常量：

```text
/auth/check
/config
/metrics/report
/clients/report
```

不接受客户端动态提供 path。

---

## 6.2 请求 Header

示例：

```http
POST /auth/check HTTP/1.1
Host: control-plane:8080
Content-Type: application/json
X-Gateway-Token: gateway-secret
Content-Length: 58
Connection: close

{"client_id":"client-A","token":"client-secret"}
```

必须验证：

```text
host_ 不包含 \r 或 \n
gateway_token_ 不包含 \r 或 \n
path 不包含 \r 或 \n
port 范围为 1～65535
```

防止环境变量或配置值造成 Header Injection。

如果未来允许 IPv6 字面量，Host Header 应使用：

```text
[IPv6-address]:port
```

---

## 6.3 Go 响应契约

Go 控制面所有内部 JSON 响应必须：

- 先 `json.Marshal()`；
- 显式设置 `Content-Type`；
- 显式设置 `Content-Length`；
- 再调用 `WriteHeader()`；
- 最后写入完整 Body。

当前 Go `writeJSON()` 直接在 `WriteHeader()` 后使用 `json.NewEncoder()`，没有显式固定响应 Body 和 Content-Length。

目标实现：

```go
func writeJSON(
    w http.ResponseWriter,
    statusCode int,
    payload any,
) {
    body, err := json.Marshal(payload)
    if err != nil {
        writeInternalErrorBeforeCommit(w)
        return
    }

    w.Header().Set("Content-Type", "application/json")
    w.Header().Set(
        "Content-Length",
        strconv.Itoa(len(body)),
    )
    w.WriteHeader(statusCode)

    if _, err := w.Write(body); err != nil {
        log.Printf("write json response failed: %v", err)
    }
}
```

---

## 6.4 C++ 响应解析规则

C++ Client 必须要求：

```text
1. 存在完整状态行
2. HTTP 版本合法
3. 状态码位于 100～599
4. Header 以 \r\n\r\n 结束
5. Header 名大小写不敏感
6. 恰好存在一个合法 Content-Length
7. 不允许 Transfer-Encoding
8. 不允许 Content-Length 与 Transfer-Encoding 共存
9. Body 大小与 Content-Length 一致
10. Body 不超过最大限制
```

Header 名大小写不敏感：

```text
Content-Length
content-length
CONTENT-LENGTH
```

均视为同一 Header。

Content-Length 值必须：

- 去除两端空白；
- 只包含十进制数字；
- 不允许正负号；
- 不允许尾随字符；
- 不允许溢出；
- 不允许超过 Body 上限。

---

## 6.5 固定大小限制

推荐：

```text
MAX_HTTP_HEADER_BYTES = 16 KiB
MAX_HTTP_BODY_BYTES   = 1 MiB
```

Header 超出限制：

```text
HttpError::HeaderTooLarge
```

Body 声明超过限制：

```text
HttpError::BodyTooLarge
```

未读满 Content-Length 就 EOF：

```text
HttpError::PrematureEof
```

当前已接收数据超过声明 Content-Length：

```text
HttpError::MalformedResponse
```

---

## 6.6 明确不支持的 HTTP 特性

发现以下内容时明确失败：

```text
Transfer-Encoding
chunked
Upgrade
Content-Encoding
HTTP/2
重定向自动跟随
压缩响应
无 Content-Length 的响应
```

不实现“尽量兼容”。

该 Client 是固定内部协议 Client，不是通用 HTTP 库。

---

# 7. ControlPlaneClient 最终接口

## 7.1 底层错误类型

```cpp
enum class HttpError
{
    None,

    ResolveFailed,
    DeadlineExceeded,
    ConnectFailed,
    SendFailed,
    ReceiveFailed,

    HeaderTooLarge,
    BodyTooLarge,
    MalformedResponse,
    MissingContentLength,
    DuplicateContentLength,
    UnsupportedTransferEncoding,
    PrematureEof,

    HttpStatusError,
    InvalidJson,
};
```

---

## 7.2 HTTP 返回结构

```cpp
struct HttpResult
{
    HttpError error = HttpError::None;
    int status_code = 0;
    std::string body;

    bool ok() const noexcept
    {
        return error == HttpError::None &&
               status_code >= 200 &&
               status_code < 300;
    }
};
```

---

## 7.3 AUTH 业务结果

```cpp
enum class AuthOutcome
{
    Allowed,
    Denied,
    Unavailable,
};
```

```cpp
struct AuthResult
{
    AuthOutcome outcome = AuthOutcome::Unavailable;
    HttpError http_error = HttpError::None;
    int http_status = 0;
    std::string reason_code;
};
```

映射规则：

```text
2xx + 合法 JSON + allowed=true
→ Allowed

2xx + 合法 JSON + allowed=false
→ Denied

DNS / connect / timeout / send / receive 失败
→ Unavailable

非法 HTTP
→ Unavailable

非 2xx
→ Unavailable

JSON 非法
→ Unavailable

allowed 字段不存在
→ Unavailable
```

注意：

```text
HTTP 401 / 403
```

对内部 `/auth/check` 通常意味着 Gateway Token 或服务鉴权配置错误，不应当计为客户端 Token 验证失败。

---

## 7.4 对外接口

调整为：

```cpp
AuthResult checkAuth(
    const std::string &client_id,
    const std::string &token) const;

bool fetchConfig(RuntimeConfig &config) const;

bool reportMetrics(const GatewayMetrics &metrics) const;

bool reportClients(
    const std::string &gateway_id,
    const std::vector<ClientReport> &clients) const;
```

`fetchConfig()`、`reportMetrics()` 和 `reportClients()` 可以暂时继续返回 `bool`，但底层必须使用同一个 `HttpResult` 和错误分类。

日志至少需要区分：

```text
resolve
deadline
connect
send
receive
protocol
http_status
json
oversize
```

日志不得输出：

- 客户端 Token；
- Gateway Token；
- Token Hash；
- 完整 Authorization Header。

---

# 8. Deadline 设计

## 8.1 使用绝对截止时间

每次 HTTP 请求创建：

```cpp
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

Deadline deadline =
    Clock::now() +
    std::chrono::milliseconds(timeout_ms_);
```

connect、send、receive 共用这一时间点。

---

## 8.2 每次等待计算剩余时间

```cpp
std::optional<int> remainingMilliseconds(
    Deadline deadline);
```

语义：

```text
deadline <= now
→ 已超时

否则
→ 返回 deadline - now
```

所有 `poll()` 前重新计算剩余时间。

---

## 8.3 EINTR 不重置预算

以下系统调用被 EINTR 中断时：

```text
poll
send
recv
connect completion wait
```

允许重试，但必须重新计算：

```text
remaining = deadline - now
```

不能重新获得完整 `timeout_ms_`。

---

## 8.4 多地址共享同一 deadline

`getaddrinfo()` 返回多个地址时：

```text
地址 A connect 消耗 600ms
地址 B 只能使用剩余预算
```

不能让每个地址都独立获得 1000ms。

---

## 8.5 DNS 边界

普通同步：

```cpp
getaddrinfo()
```

无法被 Socket `poll()` deadline 强制中止。

因此项目必须精确声明：

> `CONTROL_PLANE_TIMEOUT_MS` 保证地址解析完成后，connect、send 和 receive 共享同一个绝对 deadline。

不得宣称：

> 包含 DNS 在内，整个 `checkAuth()` 严格最多耗时 timeout_ms。

当前阶段不引入：

- c-ares；
- resolver 线程池；
- `getaddrinfo_a`；
- 异步 DNS 框架。

可以记录 DNS 耗时指标，但将其保留为已知边界。

---

# 9. Socket 状态机

Socket 从创建到关闭始终保持非阻塞。

```text
socket(SOCK_NONBLOCK | SOCK_CLOEXEC)
        ↓
connect()
        ├── 立即成功
        └── EINPROGRESS
                ↓
            poll(POLLOUT)
                ↓
            getsockopt(SO_ERROR)
        ↓
send()
        ├── 发送部分数据
        ├── EINTR：重试
        └── EAGAIN：poll(POLLOUT)
        ↓
recv()
        ├── 收到数据
        ├── EINTR：重试
        ├── EAGAIN：poll(POLLIN)
        └── EOF：按协议状态判断
```

不再使用：

```text
select()
SO_SNDTIMEO
SO_RCVTIMEO
恢复阻塞模式
```

---

# 10. `poll()` 辅助接口

建议统一封装：

```cpp
enum class WaitEvent
{
    Readable,
    Writable,
};
```

```cpp
HttpError waitForSocket(
    int fd,
    WaitEvent event,
    Deadline deadline);
```

处理：

```text
POLLIN
POLLOUT
POLLERR
POLLHUP
POLLNVAL
EINTR
deadline
```

connect 完成后必须继续：

```cpp
getsockopt(
    fd,
    SOL_SOCKET,
    SO_ERROR,
    ...
);
```

`poll(POLLOUT)` 只代表连接产生结果，不代表一定成功。

---

# 11. `sendAllWithDeadline()`

接口：

```cpp
HttpError sendAllWithDeadline(
    int fd,
    std::string_view data,
    Deadline deadline);
```

行为：

```text
send > 0
→ 增加 offset

send == -1 && EINTR
→ 重新计算 deadline 后重试

send == -1 && EAGAIN/EWOULDBLOCK
→ poll(POLLOUT, remaining)

其他错误
→ SendFailed
```

继续使用：

```cpp
MSG_NOSIGNAL
```

避免对端提前关闭时触发 SIGPIPE。

---

# 12. `readHttpResponseWithDeadline()`

读取过程分两阶段。

## 12.1 阶段一：读取 Header

不断读取，直到找到：

```text
\r\n\r\n
```

期间检查：

```text
累计 Header 字节数 <= MAX_HTTP_HEADER_BYTES
```

Header 完整后解析：

- 状态行；
- Headers；
- Content-Length；
- Transfer-Encoding；
- Header 重复；
- Body 已经随同第一批数据收到的部分。

---

## 12.2 阶段二：读取固定 Body

如果：

```text
Content-Length = N
```

则必须读取：

```text
恰好 N 字节
```

读满后立即返回，不再依赖 EOF。

行为：

```text
收到字节数 < N 且 EOF
→ PrematureEof

收到数据使累计字节数 > N
→ MalformedResponse

N > MAX_HTTP_BODY_BYTES
→ BodyTooLarge

收到恰好 N
→ 成功
```

`Connection: close` 继续保留，但它只负责连接生命周期，不再作为唯一消息边界。

---

# 13. AuthTask 设计

建议新增：

```cpp
struct AuthCancellation
{
    std::atomic<bool> cancelled{false};
};
```

```cpp
struct AuthTask
{
    Request request;
    std::shared_ptr<AuthCancellation> cancellation;
};
```

构造 AuthTask 时必须保证：

```cpp
request.type == MessageType::AUTH
```

可通过集中工厂实现：

```cpp
std::optional<AuthTask> makeAuthTask(
    Request request,
    std::shared_ptr<AuthCancellation> cancellation);
```

普通业务仍使用：

```cpp
BlockQueue<Request> request_queue_;
```

AUTH 使用：

```cpp
BlockQueue<AuthTask> auth_queue_;
```

---

# 14. Connection 中的 AUTH 状态

Connection 增加：

```cpp
std::shared_ptr<AuthCancellation> auth_cancellation;
```

当 Reactor 接收到合法首个 AUTH：

```text
authenticated == false
auth_pending == false
```

执行：

```text
auth_pending = true
创建 AuthCancellation
保存到 Connection
构造 AuthTask
尝试放入 auth_queue_
```

---

# 15. AUTH 路由

现有代码将所有请求放入公共 `request_queue_`。公共队列满时，已经能构造本地 503，并对 AUTH 使用 `AUTH_RESP`。

重构后：

```cpp
if (request.type == MessageType::AUTH)
{
    routeAuthRequest(std::move(request));
}
else
{
    routeNormalRequest(std::move(request));
}
```

---

## 15.1 AUTH Queue 入队成功

```text
auth_queue_.push(task) == OK
→ 等待 Auth Worker
```

---

## 15.2 AUTH Queue 满或停止

```text
auth_queue_.push(task) != OK
→ incrementAuthQueueRejected()
→ 构造 503 AUTH_RESP
→ close_connection=true
→ Reactor 本地 applyResponse()
```

不得经过共享 Response Queue。

这样可以避免：

```text
AUTH Queue 已过载
→ 又向 Response Queue 增加额外压力
```

同时 `applyResponse()` 会清除：

```text
auth_pending
```

当前 `applyResponse()` 已经在 AUTH_RESP 或关闭响应时清除该状态。

---

# 16. Auth Worker

新增：

```cpp
std::vector<std::thread> auth_workers_;
```

循环：

```cpp
void TcpServer::authWorkerLoop(
    unsigned int worker_id)
{
    AuthTask task;

    while (auth_queue_.pop(task))
    {
        if (task.cancellation &&
            task.cancellation->cancelled.load(
                std::memory_order_relaxed))
        {
            incrementAuthTaskCancelledBeforeStart();
            continue;
        }

        auth_in_flight_.fetch_add(
            1,
            std::memory_order_relaxed);

        auto guard = makeScopeExit([this]
        {
            auth_in_flight_.fetch_sub(
                1,
                std::memory_order_relaxed);
        });

        try
        {
            AuthResult result =
                control_plane_.checkAuth(
                    extractClientId(task.request),
                    extractToken(task.request));

            Response response =
                business::makeAuthResponse(
                    task.request,
                    result);

            enqueueWorkerResponse(
                std::move(response),
                ResponseProducer::AuthWorker);
        }
        catch (...)
        {
            Response response =
                makeInternalAuthErrorResponse(
                    task.request);

            enqueueWorkerResponse(
                std::move(response),
                ResponseProducer::AuthWorker);
        }
    }

    onResponseProducerExited();
}
```

Auth Worker 直接调用认证逻辑，不走通用 Dispatcher。

普通 Worker 不再处理 AUTH。

---

# 17. 陈旧 AUTH 任务取消

问题：

```text
客户端提交 AUTH
→ AUTH 排队
→ 客户端立即断开
→ Auth Worker 后续仍访问 Go
```

`fd + conn_id` 可以保证陈旧响应不会错误应用，但无法避免无意义的控制面调用。

处理方式：

```text
Connection 关闭
→ Reactor 将 cancellation.cancelled=true
```

Auth Worker 在开始 HTTP 前检查。

禁止让 Auth Worker 直接调用：

```cpp
isCurrentAuthPending(fd, conn_id)
```

并读取 `connections_`，否则会削弱 Reactor 独占 Connection 的架构原则。

取消令牌只是性能优化，不是正确性基础。

即使检查后连接立刻断开，最终仍由：

```text
fd + conn_id
```

保证响应安全。

新增指标：

```text
auth_tasks_cancelled_before_start_total
```

---

# 18. 普通 Worker

普通 Worker 继续处理：

```text
PING
ECHO
LOG_PUSH
STATS
```

如果错误地收到 AUTH：

```text
记录 invariant violation
生成内部错误或直接拒绝
```

测试必须保证：

```text
普通 Worker 不会调用 handleAuth
Auth Worker 不会处理普通 Request
```

---

# 19. Response Queue 生命周期

当前最后一个普通 Worker 退出时会直接：

```cpp
response_queue_.stop();
notifier_.notify();
```



增加 Auth Worker 后必须统一计算所有 Response 生产者：

```cpp
response_producers_remaining_ =
    normal_worker_count +
    auth_worker_count;
```

普通 Worker 和 Auth Worker 退出时统一调用：

```cpp
void TcpServer::onResponseProducerExited()
{
    if (response_producers_remaining_.fetch_sub(
            1,
            std::memory_order_acq_rel) == 1)
    {
        response_queue_.stop();
        notifier_.notify();
    }
}
```

只有最后一个 Response Producer 退出后，才允许停止 Response Queue。

---

# 20. Response 来源分类

新增：

```cpp
enum class ResponseProducer
{
    NormalWorker,
    AuthWorker,
};
```

接口：

```cpp
bool enqueueWorkerResponse(
    Response response,
    ResponseProducer producer);
```

指标分别记录：

```text
response_queue_rejected_normal_total
response_queue_rejected_auth_total
```

仍然共用同一条 Response Queue。

只有压测证明响应侧出现明显相互污染，才考虑双 Response Queue。

---

# 21. Connection closing 事件修正

当前 `connectionEvents()` 只根据 ServerState 和 wants_write 决定是否附加 EPOLLIN。

而 `applyResponse()` 设置 `current.closing=true` 后，仍可能继续监听 EPOLLIN。

重构为：

```cpp
uint32_t TcpServer::connectionEvents(
    bool wants_write,
    bool closing) const
{
    uint32_t events = CLIENT_BASE_EVENTS;

    if (state_.load() == ServerState::RUNNING &&
        !closing)
    {
        events |= EPOLLIN;
    }

    if (wants_write)
    {
        events |= EPOLLOUT;
    }

    return events;
}
```

或者：

```cpp
uint32_t connectionEvents(
    const Connection &connection) const;
```

规则：

```text
Connection.closing == true
→ 不再监听 EPOLLIN
→ 只允许发送剩余输出
→ 写完关闭
```

防止客户端在 503 或拒绝响应尚未写完时继续提交数据。

---

# 22. Shutdown 状态机

## 22.1 进入 DRAINING

执行顺序：

```text
RUNNING
→ DRAINING
→ readiness 失败
→ 关闭 Listener
→ 停止接收新连接
→ request_queue_.stop()
→ auth_queue_.stop()
→ 已入队任务继续处理
→ Response 继续回写
```

当前 `beginDraining()` 只停止普通 Request Queue。

重构后必须同时：

```cpp
request_queue_.stop();
auth_queue_.stop();
```

---

## 22.2 正常排空完成

排空条件至少包括：

```text
response_producers_remaining == 0
request_queue 已停止且为空
auth_queue 已停止且为空
response_queue 已停止且为空
没有待发送 output buffer
没有待处理 rejected response
```

---

## 22.3 达到强制截止时间

执行：

```cpp
const size_t discarded_normal =
    request_queue_.abort();

const size_t discarded_auth =
    auth_queue_.abort();

const size_t discarded_responses =
    response_queue_.abort();
```

记录：

```text
discarded_normal_requests
discarded_auth_tasks
discarded_responses
remaining_connections
auth_in_flight
```

注意：

> `auth_queue_.abort()` 只能删除尚未开始的任务，不能取消已经进入同步 HTTP 调用的 Auth Worker。

因此必须先完成 HTTP Client 的共享 deadline，再引入 Auth Executor。

---

## 22.4 后台线程 shutdown

以下线程也会调用同步 ControlPlaneClient：

```text
metrics reporter
config puller
```

严格 deadline 必须同样应用于：

```text
fetchConfig
reportMetrics
reportClients
```

否则 shutdown 仍可能卡在：

```cpp
metrics_reporter_.join();
config_puller_.join();
```

当前 `finishShutdown()` 确实会等待这些线程。

周期线程不得使用无法打断的长时间 `sleep_for()`。

推荐：

```cpp
condition_variable.wait_for(...)
```

shutdown 时：

```text
设置 stop flag
→ notify_all()
→ 周期线程立即醒来并退出
```

---

# 23. AUTH 响应语义

建议 Go `/auth/check` 对业务认证决策统一返回 200：

认证成功：

```json
{
  "allowed": true,
  "code": "OK"
}
```

凭证错误：

```json
{
  "allowed": false,
  "code": "INVALID_CREDENTIALS"
}
```

Token 被禁用：

```json
{
  "allowed": false,
  "code": "TOKEN_DISABLED"
}
```

触发安全限速：

```json
{
  "allowed": false,
  "code": "RATE_LIMITED"
}
```

控制面或 Redis 不可用：

```http
HTTP/1.1 503 Service Unavailable
```

C++ 映射：

```text
200 + allowed=true
→ Allowed

200 + allowed=false
→ Denied

503
→ Unavailable
```

---

# 24. Redis AUTH 失败限速

该逻辑属于 Go 控制面，不属于 C++ Gateway。

计划原本要求使用：

```text
auth:failures:{identity}
```

并实现 TTL 计数、临时拒绝和成功清理。

---

## 24.1 Identity

初始版本可使用：

```text
client_id
```

Redis Key：

```text
auth:failures:{client_id}
```

不建议直接使用 Token 作为 Key。

---

## 24.2 计数条件

只有以下情况才增加失败次数：

```text
Redis 操作成功
Token Record 查询成功
Token 不存在
Token disabled
Token Hash 不匹配
```

以下情况不得增加失败次数：

```text
Redis 查询失败
Redis 超时
Go 内部错误
请求 JSON 非法
Gateway Token 错误
网络错误
C++ AUTH Queue 满
C++ HTTP deadline
```

---

## 24.3 推荐算法

Redis Lua 或原子命令流程：

```text
读取 auth:failures:{client_id}
→ 如果已达到阈值且 TTL 未过期
    → 返回 Rate Limited
→ 执行 Token 验证
→ 验证失败
    → INCR
    → 首次失败时设置 TTL
→ 验证成功
    → DEL 或按策略衰减
```

示例参数：

```text
失败阈值：5
窗口 TTL：60 秒
临时拒绝：由剩余 TTL 决定
```

这些值为演示参数，应允许通过 Go 启动配置调整。

---

# 25. Metrics

## 25.1 C++ AUTH Executor 指标

新增：

```text
auth_queue_capacity
auth_queue_backlog
auth_queue_peak
auth_queue_rejected_total
auth_in_flight
auth_tasks_cancelled_before_start_total
auth_allowed_total
auth_denied_total
auth_unavailable_total
auth_duration_seconds
```

---

## 25.2 HTTP Client 指标

建议按低基数 error category 记录：

```text
control_plane_requests_total{
    operation,
    result
}

control_plane_request_duration_seconds{
    operation
}

control_plane_errors_total{
    operation,
    category
}
```

operation：

```text
auth
config
metrics_report
clients_report
```

category：

```text
resolve
deadline
connect
send
receive
protocol
status
json
oversize
```

禁止标签：

```text
client_id
request_id
remote_addr
token
raw path
```

---

## 25.3 Response Queue 来源指标

新增：

```text
response_queue_rejected_normal_total
response_queue_rejected_auth_total
```

---

## 25.4 Go AUTH 限速指标

新增：

```text
control_plane_auth_total{
    result
}

control_plane_auth_rate_limited_total
control_plane_auth_failure_counter_errors_total
```

result：

```text
allowed
denied
unavailable
```

---

# 26. 测试设计

# 26.1 ControlPlaneClient 单元与假服务端测试

建立本地 Fake HTTP Server，能够精确控制响应字节和时间。

必须覆盖：

### 正常响应

```text
合法 200
合法 Content-Length
合法 JSON
allowed=true
```

预期：

```text
AuthOutcome::Allowed
```

### 凭证拒绝

```text
200
allowed=false
```

预期：

```text
AuthOutcome::Denied
```

### 非 2xx

```text
500
503
401
403
```

预期：

```text
AuthOutcome::Unavailable
```

### JSON 非法

预期：

```text
Unavailable
HttpError::InvalidJson
```

### allowed 缺失

预期：

```text
Unavailable
```

### Header 超长

预期：

```text
HeaderTooLarge
```

### Body 声明超长

预期：

```text
BodyTooLarge
```

### 提前 EOF

```text
Content-Length: 100
实际只返回 30 字节
```

预期：

```text
PrematureEof
```

### 重复 Content-Length

预期：

```text
DuplicateContentLength
```

### Transfer-Encoding

```http
Transfer-Encoding: chunked
```

预期：

```text
UnsupportedTransferEncoding
```

### Transfer-Encoding 与 Content-Length 共存

预期：

```text
MalformedResponse
```

### 慢速滴答响应

```text
deadline=1000ms
服务端每 900ms 发送少量数据
```

预期：

```text
总 connect/send/receive 时间受共享 deadline 限制
```

### 多地址共享 deadline

验证地址 A 消耗时间后，地址 B 只能使用剩余预算。

### EINTR

验证重试不会重置完整超时。

### 高 fd

测试前打开大量 dummy fd，使 HTTP Socket fd 超过常见 `FD_SETSIZE`。

预期：

```text
poll 正常工作
不存在 FD_SET 风险
```

### CRLF 注入

host 或 gateway token 含：

```text
\r
\n
```

预期：

```text
启动配置或 Client 构造失败
```

---

# 26.2 Auth Executor 测试

### 路由隔离

验证：

```text
AUTH 只进入 auth_queue
ECHO 只进入 request_queue
```

### 执行隔离

验证：

```text
普通 Worker 不调用 checkAuth
Auth Worker 不处理 ECHO
```

### AUTH 并发上限

例如：

```text
Auth Worker=2
同时提交 20 个 AUTH
```

验证同时调用控制面的最大数量不超过 2。

### AUTH Queue 上限

```text
Auth Queue Capacity=4
```

超出部分：

```text
快速返回 503 AUTH_RESP
```

### 普通业务不受 AUTH 饱和影响

持续压满 AUTH Queue 和 Auth Worker，同时发送 ECHO。

验证：

```text
普通 Worker 仍能处理 ECHO
普通 Request Queue 未被 AUTH 填满
```

### 客户端断开

```text
AUTH 入队
→ 客户端断开
→ Auth Worker 尚未开始
```

验证：

```text
任务被 cancellation 跳过
指标增加
不调用 Go
```

### 竞态断开

```text
Auth Worker 检查 cancellation 后
客户端立即断开
```

验证：

```text
可能仍调用 Go
但最终 Response 被 fd + conn_id 丢弃
```

---

# 26.3 Shutdown 测试

### 正常排空

```text
普通任务 + AUTH 任务正在排队
→ SIGTERM
```

验证：

```text
两个输入 Queue 同时 stop
已入队任务继续处理
最后一个 Response Producer 停止 Response Queue
所有响应正常 drain
```

### Auth Worker 慢请求

```text
Auth Worker 正在 HTTP 调用
→ SIGTERM
```

验证：

```text
HTTP 在共享 deadline 内返回
join 不无限等待
```

### 强制截止

```text
控制面持续不响应
shutdown deadline 到达
```

验证：

```text
request_queue abort
auth_queue abort
response_queue abort
记录丢弃数量
Gateway 退出
```

### 配置拉取和指标线程

验证 shutdown 能打断周期等待，并且后台 HTTP 调用受 deadline 限制。

---

# 26.4 故障与恢复测试

测试流程：

```text
1. 正常启动 Gateway、Go、Redis
2. 创建 Token
3. AUTH 成功
4. 保持已认证 ECHO 连接
5. 停止 Go 控制面
6. 新 AUTH 返回失败
7. 已认证 ECHO 继续
8. 普通 Worker 不被 Auth Worker 占满
9. 恢复 Go
10. 新 AUTH 自动恢复
11. 配置拉取恢复
12. 指标上报恢复
```

开发计划原本要求验证控制面不可用、已认证连接继续、新 AUTH 失败，以及恢复后配置和上报恢复。

---

# 26.5 Redis 安全限速测试

覆盖：

```text
连续非法 Token
达到阈值后临时拒绝
TTL 到期后恢复
成功认证后失败计数清理
Redis 错误不增加失败次数
并发失败不丢计数
Rate Limited 返回稳定 code
```

---

# 27. 分 Push 实施计划

# Push 5.6.1：冻结 HTTP 和 AUTH 结果契约

新增或修改：

```text
docs/api_contract.md
docs/control_plane_client.md
docs/failure_semantics.md
```

定义：

- Go/C++ HTTP 响应契约；
- Content-Length 规则；
- 不支持项；
- Header/Body 上限；
- `HttpError`；
- `AuthOutcome`；
- deadline 精确语义；
- DNS 已知边界；
- 环境变量。

检查：

```text
仅文档变更
现有测试全部通过
```

---

# Push 5.6.2：Go 显式 Content-Length

修改：

```text
go-control-plane/internal/app/handlers.go
相关 Handler Tests
```

实现：

- `json.Marshal()`；
- 显式 Content-Length；
- 统一 Marshal 失败路径；
- Body 不额外追加换行；
- Handler 测试校验 Header 和 Body。

检查：

```bash
cd go-control-plane
go test ./...
go test -race ./...
go vet ./...
```

---

# Push 5.6.3：ControlPlaneClient 类型和启动配置

修改：

```text
ControlPlaneClient.hpp
ControlPlaneClient.cpp
main.cpp / startup config
TcpServer constructor
```

实现：

- `HttpError`；
- `HttpResult`；
- `AuthOutcome`；
- `AuthResult`；
- `CONTROL_PLANE_TIMEOUT_MS`；
- host/token CRLF 检查；
- 配置范围校验。

此 Push 暂时不修改底层 I/O 状态机。

---

# Push 5.6.4：poll 与共享 deadline

实现：

- Socket 全程非阻塞；
- `poll()`；
- `waitForSocket()`；
- `remainingMilliseconds()`；
- 多地址共享 deadline；
- connect 后检查 `SO_ERROR`；
- 删除 `select()`；
- 删除 `SO_RCVTIMEO`；
- 删除 `SO_SNDTIMEO`；
- 删除恢复阻塞模式。

---

# Push 5.6.5：严格 HTTP Framing

实现：

- Header 限制；
- Header 解析；
- Content-Length；
- Body 固定读取；
- 提前 EOF；
- 重复 Content-Length；
- Transfer-Encoding 拒绝；
- Body 大小限制；
- 非 2xx 分类；
- JSON 错误分类。

---

# Push 5.6.6：ControlPlaneClient 假服务端测试

新增完整测试：

- 正常；
- 慢速；
- 超时；
- 提前 EOF；
- chunked；
- 高 fd；
- Header 注入；
- 多地址；
- 非法 JSON；
- 非 2xx。

完成后才允许开始 Auth Executor。

---

# Push 5.7.1：AuthTask 与独立有界队列

新增：

```text
AuthTask
AuthCancellation
auth_queue_
auth_workers_
AUTH_WORKER_COUNT
AUTH_QUEUE_CAPACITY
```

修改请求路由：

```text
AUTH → auth_queue
其他 → request_queue
```

AUTH Queue 满：

```text
Reactor 本地生成 503 AUTH_RESP
```

---

# Push 5.7.2：双 Worker Group 生命周期

实现：

- `response_producers_remaining_`；
- 普通 Worker 和 Auth Worker 统一退出回调；
- 最后一个生产者 stop Response Queue；
- `beginDraining()` 同时 stop 两个输入 Queue；
- 强制截止同时 abort 三个 Queue；
- join 所有 Worker；
- 后台 reporter/puller 可中断等待。

---

# Push 5.7.3：陈旧任务与 closing 边界

实现：

- AuthCancellation；
- Connection 关闭设置 cancelled；
- Auth Worker 开始前跳过；
- cancelled 指标；
- closing 时移除 EPOLLIN；
- 对应 CTest。

---

# Push 5.7.4：AUTH 指标与联合压力测试

实现：

- Auth Queue backlog/capacity/peak/rejected；
- auth in-flight；
- allowed/denied/unavailable；
- HTTP latency；
- Response Queue reject 来源分类；
- AUTH 洪峰下 ECHO 测试；
- shutdown 联合测试；
- 控制面恢复闭环测试。

---

# 补做 Push 2.5：Redis AUTH 失败限速

实现：

- `auth:failures:{identity}`；
- TTL；
- 阈值；
- Rate Limited；
- 成功清理；
- Redis 故障不误计；
- 并发测试；
- Prometheus 指标。

虽然开发顺序位于 Phase 5 Completion 之后，但逻辑归属仍为原 Phase 2.5。

---

# 28. 预计修改文件

C++：

```text
cpp-gateway/include/control/ControlPlaneClient.hpp
cpp-gateway/src/control/ControlPlaneClient.cpp
cpp-gateway/include/net/TcpServer.hpp
cpp-gateway/src/net/TcpServer.cpp
cpp-gateway/include/business/AuthTask.hpp
cpp-gateway/src/business/Handlers.cpp
cpp-gateway/include/business/StatsManager.hpp
cpp-gateway/src/business/StatsManager.cpp
cpp-gateway/src/main.cpp
cpp-gateway/CMakeLists.txt
```

测试：

```text
cpp-gateway/tests/control_plane_client_test.cpp
cpp-gateway/tests/auth_executor_test.cpp
cpp-gateway/tests/auth_shutdown_test.cpp
cpp-gateway/tests/auth_overload_test.cpp
cpp-gateway/tests/control_plane_recovery_test.cpp
```

Go：

```text
go-control-plane/internal/app/handlers.go
go-control-plane/internal/app/handlers_test.go
go-control-plane/internal/store/*
go-control-plane/integration/auth_rate_limit_test.go
```

文档：

```text
docs/api_contract.md
docs/architecture.md
docs/design_decisions.md
docs/testing.md
docs/failure_behavior.md
docs/current_state.md
gateway_system_v2_development_plan.md
README.md
```

---

# 29. 完整验收标准

完成本重构必须满足：

1. Reactor 永远不调用控制面 HTTP；
2. 普通 Worker 不执行 AUTH；
3. Auth Worker 不执行普通业务；
4. Auth Worker 数量固定且可配置；
5. Auth Queue 有固定容量；
6. Auth Queue 满时 Reactor 本地快速拒绝；
7. AUTH 洪峰不能填满普通 Request Queue；
8. AUTH 洪峰下普通 ECHO 仍可处理；
9. connect/send/receive 共用一个绝对 deadline；
10. EINTR 不重置完整 timeout；
11. 多地址共享同一 deadline；
12. 使用 `poll()`，不再使用 `select()`；
13. Socket 全程非阻塞；
14. HTTP 响应按 Content-Length 读取；
15. 不再依赖 EOF 判断正常响应结束；
16. 提前 EOF 明确失败；
17. Header 和 Body 分别有大小限制；
18. Transfer-Encoding 明确拒绝；
19. 重复或非法 Content-Length 明确失败；
20. host 和 gateway token 拒绝 CR/LF；
21. Allowed、Denied、Unavailable 明确区分；
22. Redis 故障不计入暴力猜测失败；
23. Token 不匹配计入 Redis TTL 限速；
24. 控制面不可用时新 AUTH Fail Closed；
25. 已认证连接在控制面故障时继续工作；
26. 控制面恢复后新 AUTH 自动恢复；
27. 配置拉取和指标上报自动恢复；
28. 连接断开后的排队 AUTH 可在开始前取消；
29. 最终仍通过 fd + conn_id 防止陈旧响应；
30. closing 连接不再监听 EPOLLIN；
31. 最后一个 Response Producer 才停止 Response Queue；
32. shutdown 同时停止普通和 AUTH Queue；
33. 强制截止能中止所有待处理队列；
34. 所有同步 HTTP 调用都有 deadline；
35. reporter 和 puller 周期等待可被 shutdown 打断；
36. 指标能够观察 AUTH Queue、in-flight、latency 和错误分类；
37. CTest、ASan、UBSan、Go Race 和 CI 全部通过。

---

# 30. 明确保留的系统边界

完成重构后，仍然不保证：

```text
包含 DNS 在内的严格端到端 timeout
极限 AUTH 吞吐量
控制面高可用
Redis 高可用
不可信公网传输安全
HTTP 通用兼容性
控制面连接复用
认证缓存命中
```

具体边界：

## DNS

同步 `getaddrinfo()` 可能超过配置 deadline。

## 每次新建连接

每次调用仍然：

```text
解析地址
→ 新建 TCP Socket
→ HTTP 请求
→ close
```

存在握手开销。

## AUTH Executor 也可能饱和

当：

```text
Auth Worker 全忙
Auth Queue 已满
```

新 AUTH 会快速拒绝。

系统目标不是消灭过载，而是：

```text
让过载有界
让过载可观测
让过载不扩散
```

## 无 TLS

项目固定范围不做 TLS。

因此只适合作为：

```text
本地 Docker
受控 Kubernetes 集群内部网络
学习与演示环境
```

不得宣称可直接在不可信公网安全传输 Token。

---

# 31. README 中的设计说明

最终 README 应明确写出：

> Gateway 数据面面对大量长期 TCP 连接，因此使用单 Reactor、epoll ET 和 eventfd。控制面调用数量较少且存在不可预测的远程延迟，因此不进入 Reactor，而是使用独立有界 AUTH Executor。

> ControlPlaneClient 保持同步接口，但内部 Socket 全程非阻塞，并使用 poll 和共享绝对 deadline。该 Client 只支持固定内部 HTTP 契约，不尝试实现通用 HTTP/1.1。

> 独立 Auth Worker 和 Auth Queue 形成 Bulkhead。控制面延迟和 AUTH 洪峰不会占用普通业务 Worker 或普通 Request Queue。

> AUTH Queue 满、控制面不可用、Redis 不可用和非法凭证均不会导致认证成功，但会通过不同结果和指标进行区分。

> 生产系统通常应优先使用成熟 HTTP 库。本项目手写最小 Client，是为了展示 Linux Socket、deadline、HTTP framing 和故障边界，而不是替代 libcurl。

---

# 32. 面试讲解链路

```text
客户端发送 AUTH
→ Reactor 解码
→ 验证当前连接未认证且没有 auth_pending
→ 创建 AuthTask 和 cancellation token
→ 放入有界 Auth Queue
→ Auth Worker 取出任务
→ 检查任务是否已取消
→ 调用严格同步 ControlPlaneClient
→ getaddrinfo
→ 非阻塞 connect
→ poll 等待完成
→ SO_ERROR 检查
→ sendAllWithDeadline
→ read Header
→ 校验 Content-Length
→ 读取固定 Body
→ 解析 JSON
→ 生成 Allowed / Denied / Unavailable
→ Auth Worker 生成 Response
→ 放入共享 Response Queue
→ eventfd 唤醒 Reactor
→ Reactor 校验 fd + conn_id
→ 更新 authenticated / client_id
→ 回写 AUTH_RESP
```

故障链路：

```text
Go 延迟升高
→ 只占用固定数量 Auth Worker
→ Auth Queue 有界积压
→ Queue 满时快速拒绝
→ 普通 Worker 和普通 Request Queue 继续工作
```

Shutdown 链路：

```text
SIGTERM
→ DRAINING
→ Listener 关闭
→ Readiness 失败
→ 普通 Queue stop
→ Auth Queue stop
→ 已入队任务继续
→ HTTP 请求受 deadline 限制
→ 最后一个 Response Producer stop Response Queue
→ Reactor 排空 Response
→ 截止时间内退出
→ 超时后 abort 剩余队列
```

---

# 33. 学习顺序

开发完成后，建议按以下顺序学习：

```text
第一步：
为什么同步 HTTP 不能进入 Reactor

第二步：
为什么独立 Auth Queue/Worker 比公共 Worker 中的并发计数更清晰

第三步：
非阻塞 connect + poll + SO_ERROR

第四步：
绝对 deadline 与单次 timeout 的区别

第五步：
部分发送和 EAGAIN

第六步：
HTTP Header、Content-Length 和提前 EOF

第七步：
Allowed / Denied / Unavailable 的语义区别

第八步：
Auth Queue Full 为什么由 Reactor 本地处理

第九步：
为什么 Auth Worker 不读取 connections_

第十步：
fd + conn_id 和 cancellation token 分别解决什么问题

第十一步：
双 Worker Group 如何共同管理 Response Queue 生命周期

第十二步：
Redis 安全限速为什么只统计真实凭证失败

第十三步：
控制面故障和 Gateway shutdown 如何共同验证
```

---

# 34. 最终终止线

完成本文件中的验收后，本模块进入冻结状态。

后续不继续加入：

```text
异步 HTTP Client
io_uring
连接池
Keep-Alive
本地认证缓存
双 Response Queue
HTTP/2
TLS
通用 HTTP 功能
```

只有 Phase 9 压测提供明确证据，证明以下某项已成为真实瓶颈：

```text
TCP 建连开销
Auth Worker 并发不足
共享 Response Queue 污染
DNS 解析开销
```

才允许重新评估。

本模块最终标准是：

> 代码规模受控；
> 协议语义严格；
> 线程职责清晰；
> 故障不会无限扩散；
> 安全失败与基础设施故障可区分；
> 所有关键边界均有自动化测试和文档证据。