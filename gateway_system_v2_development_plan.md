# Gateway System v2.0 完整开发文档

> 适用仓库：`mrussss/gateway-system`  
> 前置项目：`mrussss/cpp-epoll-tcp-server`（已掌握）  
> 最终固定范围：C++ 数据面、Go 标准库控制面、Redis、Prometheus、Kubernetes 滚动更新与优雅摘流。

---

## 1. 总体结论

当前 `gateway-system` 不是空项目，也不需要推倒重写。

已经可以作为稳定底座保留的部分包括：

- C++17 非阻塞 TCP 数据面；
- `epoll` ET、`accept4`、`eventfd`；
- 自定义长度前缀协议；
- 半包、粘包和非法长度处理；
- 单 Reactor 连接所有权；
- `fd + conn_id` 世代校验；
- 有界 Request/Response Queue；
- Worker → Response Queue → eventfd → Reactor 的响应链路；
- 慢客户端输出上限；
- `RUNNING → DRAINING → STOPPED` 有截止时间的优雅退出；
- CTest、ASan、UBSan、Go Race、CI、Docker Compose、Smoke Test 和基础压测。

下一步不再重复建设纯 C++ TCP Server，而是完成四件事：

1. 修正当前 Go 控制面和 Redis 实现中的真实性问题；
2. 补齐固定范围要求的 Prometheus 指标体系；
3. 补齐 Kubernetes 部署、滚动更新和优雅摘流验证；
4. 输出可以复现的测试、故障和性能证据。

最终版本建议标记为 `v2.0.0`，因为 Token API、配置更新协议、健康检查路径和部署方式都会发生不兼容变化。

---

## 2. 当前仓库审计

### 2.1 已完成并保留

#### C++ 数据面

保留现有网络核心，不做大重构：

- Reactor 独占 Socket、连接表和 epoll 操作；
- Worker 只接收 Request 值并返回 Response 值；
- Request/Response Queue 均有容量上限；
- Response Queue 失败通过独立 `fd → conn_id` 结构通知 Reactor 关闭连接；
- eventfd 采用“通知后排空”语义；
- 输出使用 `output_buffer + write_offset`；
- 退出过程有截止时间，超时后丢弃未开始工作并强制关闭剩余连接；
- 控制面不可用时，已认证连接继续工作，新 AUTH Fail Closed。

#### 基础控制面

当前已有：

- Go 1.22 `net/http`；
- `/auth/check`；
- Token 增删查；
- Gateway 状态与客户端快照；
- Runtime Config；
- MemoryStore 和 RedisStore；
- 多 Gateway 查询；
- Docker Compose 集成。

#### 工程验证

当前已有：

- CTest；
- 严格编译警告；
- ASan/UBSan；
- `go test`、`go test -race`、`go vet`；
- push/PR CI；
- 手动 Docker Smoke Workflow；
- TCP 协议脚本；
- 基础 Benchmark 和故障说明。

### 2.2 必须修正的问题

#### P0：固定范围与仓库文档冲突

当前 README 和设计文档把 Kubernetes 排除在项目边界之外，而最终固定范围要求：

- Prometheus；
- Kubernetes；
- 滚动更新；
- readiness；
- preStop；
- SIGTERM；
- 长连接排空。

必须统一 README、设计文档、开发流程和验收标准，删除旧的“No Kubernetes”边界。

#### P0：Token 明文存储

当前 Redis 中直接保存明文 Token，并直接使用字符串相等比较。

最终必须改为：

```text
token:{client_id}
├── token_hash
├── created_at
├── updated_at
└── disabled
```

要求：

- Token 使用 `crypto/rand` 生成；
- 仅创建和轮换时返回一次；
- Redis 不保存明文；
- 使用 SHA-256 或 HMAC-SHA256 保存 Hash；
- 使用 `crypto/subtle` 常量时间比较；
- 日志不输出完整 Token。

#### P0：配置更新存在并发丢失

当前流程是：

```text
GET current config
→ Go 中 version + 1
→ SET config
```

并发更新会读取相同版本并相互覆盖。

最终改为：

```text
PUT /config
If-Match: <expected-version>
→ Redis Lua CAS
→ 成功返回新版本
→ 版本冲突返回 409
```

配置版本和完整内容必须原子更新。

#### P0：Gateway 状态没有 TTL

当前 Gateway 状态、客户端快照和索引没有 TTL，离线实例会长期残留。

最终要求：

- `gateway:status:{gateway_id}` 设置 TTL；
- `gateway:clients:{gateway_id}` 设置 TTL；
- 上报时刷新 TTL；
- `gateway:index` 中的失效成员在查询或后台清理时移除；
- 在线状态由最新上报时间和 TTL 共同判断。

#### P0：Redis 写入不是 Pipeline

当前状态、索引和兼容 Key 分多次独立写入。

最终上报使用 Pipeline 减少 RTT，但文档必须明确：

> Pipeline 只减少网络往返，不提供事务原子性。

配置 CAS 使用 Lua，不使用 Pipeline 冒充原子操作。

#### P0：Go HTTP 工程能力不足

当前缺少：

- `/health/live`；
- `/health/ready`；
- `/metrics`；
- Request ID；
- 结构化访问日志；
- Panic Recovery；
- Body 大小限制；
- Content-Type 检查；
- 统一错误格式；
- 管理接口鉴权；
- 内部 Gateway 接口鉴权；
- HTTP 优雅退出；
- Store Close；
- 严格单 JSON 值解码。

#### P0：动态配置字段不一致

当前配置包含：

- `auth_timeout_ms`；
- `fail_open`。

但：

- `auth_timeout_ms` 没有真正动态更新 C++ HTTP Client；
- `fail_open` 实际没有启用，故障文档也声明新 AUTH 仍然 Fail Closed。

这两个字段应从动态配置删除。

最终动态配置固定为：

```text
version
max_payload_size
max_connections_per_client
max_requests_per_client_per_second
slow_client_output_limit
log_level
request_queue_capacity_display
```

其中：

- `slow_client_output_limit` 真正控制 C++ 输出缓冲上限；
- `log_level` 支持原子更新；
- `request_queue_capacity_display` 只用于展示当前启动值，不伪装成运行时可调整容量；
- 控制面 HTTP 超时改为进程启动环境变量，不属于动态业务配置。

#### P0：上报字段不足

当前状态上报仅包含：

- 连接数；
- 请求数；
- 收发字节；
- 错误数。

最终还要上报：

- Request Queue backlog/capacity/peak/rejected；
- Response Queue backlog/capacity/peak/rejected；
- 慢客户端关闭数；
- 过期响应丢弃数；
- AUTH 成功/失败；
- 配置版本；
- Shutdown 状态；
- 上报时间。

#### P0：Redis 启动失败会直接退出 Go 进程

最终应改为：

- Redis Client 可以构造；
- Control Plane 进程正常启动；
- `/health/live` 返回存活；
- `/health/ready` 在 Redis 不可用时返回 503；
- Redis 恢复后 Ready 自动恢复；
- liveness 不依赖 Redis，避免 Redis 故障触发控制面反复重启。

#### P1：容器仍以 root 运行

最终 Docker 与 Kubernetes 需要：

- 非 root 用户；
- 只读根文件系统（可行时）；
- 明确可写日志目录或改为 stdout；
- `allowPrivilegeEscalation: false`；
- 删除不需要的 Linux capabilities；
- Resource Requests/Limits。

---

## 3. 最终系统架构

```text
TCP Client
    │
    │ 自定义长度前缀协议
    ▼
C++ Gateway Data Plane
├── Reactor
│   ├── accept4
│   ├── epoll ET
│   ├── recv / decode
│   ├── output buffer
│   ├── send / close
│   └── connection lifecycle
├── Bounded Request Queue
├── Worker Pool
├── Bounded Response Queue
├── eventfd
├── fd + conn_id
├── runtime config snapshot
├── overload / slow client protection
└── graceful drain
    │
    │ HTTP/JSON + internal shared token
    ▼
Go Control Plane
├── net/http ServeMux
├── middleware
├── token management
├── auth check
├── gateway status
├── client snapshots
├── config CAS
├── Prometheus /metrics
└── live / ready
    │
    ▼
Redis
├── token hash
├── gateway status TTL
├── client snapshot TTL
├── config Lua CAS
└── auth failure limiter

Kubernetes
├── Gateway Deployment（2 replicas）
├── Control Plane Deployment
├── Redis StatefulSet + PVC
├── Services
├── ConfigMap
├── Secret
├── PDB
├── startup/readiness/liveness probes
└── preStop + terminationGracePeriodSeconds
```

---

## 4. 固定项目边界

必须完成：

```text
C++ Data Plane
Go Standard Library Control Plane
Redis
Prometheus
Kubernetes rolling update and graceful drain
```

明确不做：

- Kafka；
- PostgreSQL/MySQL；
- Gin；
- GORM；
- HTTP 反向代理；
- TLS；
- 多 Reactor；
- 服务发现；
- Consul；
- etcd；
- Nacos；
- Service Mesh；
- Operator；
- 多集群；
- Grafana 大屏；
- 完整历史日志平台；
- 自动 Fail Open；
- 全局分布式限流。

---

## 5. Git、Commit 与 Push 规则

### 5.1 分支结构

```text
main
├── feature/p0-scope-baseline
├── feature/p1-http-foundation
├── feature/p2-token-auth
├── feature/p3-redis-state
├── feature/p4-config-cas
├── feature/p5-cpp-telemetry
├── feature/p6-prometheus
├── feature/p7-local-integration
├── feature/p8-kubernetes
└── feature/p9-release
```

规则：

- `main` 始终保持可构建、测试通过；
- 一个 Phase 对应一个 Feature Branch 和一个 PR；
- 一个 Phase 内必须有多个远程 Push；
- 每个 Push 可以包含 1–3 个本地 Commit；
- 不使用 Squash Merge，保留学习检查点；
- 合并使用普通 Merge Commit；
- 核心实现前打标签，方便后续创建学习分支。

### 5.2 核心实现检查点

示例：

```bash
git tag -a checkpoint/config-cas-before-impl -m "before config CAS implementation"
git push origin checkpoint/config-cas-before-impl

git switch -c learn/config-cas checkpoint/config-cas-before-impl
```

建议保留：

```text
checkpoint/http-middleware-before-impl
checkpoint/token-security-before-impl
checkpoint/redis-ttl-before-impl
checkpoint/config-cas-before-impl
checkpoint/cpp-metrics-before-impl
checkpoint/prometheus-before-impl
checkpoint/k8s-drain-before-impl
```

### 5.3 什么时候允许 Push

每次 Push 前至少满足：

- 当前代码能编译；
- 本阶段已经存在的测试通过；
- 没有把 Token、Secret、密码提交进仓库；
- 行为变更同步更新测试；
- 对外 API 变更同步更新文档；
- 不允许“先推坏代码，后面再修”；
- Feature Branch 可以功能未完整，但不能处于不可验证状态。

### 5.4 基础检查

```bash
cmake -S cpp-gateway -B cpp-gateway/build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build cpp-gateway/build --parallel
ctest --test-dir cpp-gateway/build --output-on-failure

cmake -S cpp-gateway -B cpp-gateway/build-sanitized \
  -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON \
  -DGATEWAY_ENABLE_SANITIZERS=ON
cmake --build cpp-gateway/build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir cpp-gateway/build-sanitized --output-on-failure

(cd go-control-plane && go test ./...)
(cd go-control-plane && go test -race ./...)
(cd go-control-plane && go vet ./...)

docker compose config
bash -n scripts/*.sh
python3 -m compileall -q scripts cpp-gateway/scripts cpp-gateway/tests
```

---

# 6. 完整开发阶段

## Phase 0：范围统一与基线冻结

分支：

```text
feature/p0-scope-baseline
```

目标：

- 确认当前代码为 v2 开发基线；
- 不改核心行为；
- 统一最终范围和 API Contract；
- 清除旧文档冲突。

### Push 0.1：保存当前基线

内容：

- 新增 `docs/current_state.md`；
- 列出已实现功能、已知问题和最终范围；
- 记录当前 HEAD SHA；
- 新增 `CHANGELOG.md`；
- 标记当前版本为 Legacy v1 Scope。

检查：

```bash
全量基础检查
```

### Push 0.2：冻结 v2 API Contract

新增：

```text
docs/api_contract.md
docs/redis_schema.md
docs/metrics_contract.md
docs/shutdown_contract.md
```

明确最终接口：

```text
GET  /health/live
GET  /health/ready
GET  /metrics

POST /auth/check
POST /metrics/report
POST /clients/report

POST   /tokens
GET    /tokens
DELETE /tokens/{client_id}
POST   /tokens/{client_id}/rotate

GET /gateways
GET /gateways/{gateway_id}/status
GET /gateways/{gateway_id}/clients

GET /config
PUT /config
```

### Push 0.3：修正文档边界

内容：

- 删除“No Kubernetes”；
- 删除“No Prometheus”；
- 保留 Kafka、数据库、Gin、多 Reactor 等不做项；
- README 增加 v2 Roadmap；
- `docs/development_workflow.md` 改为本开发流程。

完成后发起 PR，合并到 `main`。

---

## Phase 1：Go 控制面工程化基础

分支：

```text
feature/p1-http-foundation
```

目标：

- 保持 `net/http`；
- 将平铺的 `package main` 拆成可维护结构；
- 建立健康检查、中间件和优雅退出。

最终结构：

```text
go-control-plane/
├── cmd/control-plane/main.go
├── internal/
│   ├── app/
│   ├── api/
│   ├── middleware/
│   ├── auth/
│   ├── config/
│   ├── gateway/
│   ├── metrics/
│   └── store/
├── integration/
└── go.mod
```

### Push 1.1：只移动目录，不改行为

- 建立 `cmd/` 和 `internal/`；
- 通过依赖注入替换全局 `store`；
- 现有 Handler 测试迁移并保持通过。

检查：

```bash
go test ./...
go test -race ./...
go vet ./...
```

### Push 1.2：统一响应和严格 JSON 解码

实现：

- 统一错误结构；
- `DisallowUnknownFields`；
- 限制只能有一个 JSON 值；
- Body 大小限制；
- Content-Type 校验；
- 405 和 404 统一格式。

错误格式：

```json
{
  "request_id": "req-xxx",
  "code": "INVALID_ARGUMENT",
  "message": "invalid request body"
}
```

### Push 1.3：Request ID、结构化日志和 Recovery

实现：

- Request ID；
- Access Log；
- Panic Recovery；
- Status Recorder；
- 请求耗时；
- 日志字段固定。

### Push 1.4：请求超时与鉴权路由分组

建立三类路由：

```text
Public
Internal Gateway
Admin
```

先完成路由分组和接口，Token 细节在 Phase 2 实现。

### Push 1.5：live、ready 与 Store Health

实现：

```text
GET /health/live
GET /health/ready
```

规则：

- live 只代表进程与 HTTP Server 存活；
- ready 检查 Redis 和必要配置；
- Redis 一次失败只导致 Not Ready，不导致 Liveness 失败。

### Push 1.6：Go HTTP 优雅退出

使用：

```text
signal.NotifyContext
http.Server.Shutdown
Store.Close
```

测试：

- SIGTERM 后停止接收新 HTTP 请求；
- 截止时间内完成在途请求；
- 超时后退出；
- Redis Close 被调用。

核心前置标签：

```text
checkpoint/http-middleware-before-impl
```

---

## Phase 2：安全 Token 与鉴权体系

分支：

```text
feature/p2-token-auth
```

### Push 2.1：Token 领域模型与 Hash

实现：

- `TokenRecord`；
- Token 生成；
- Hash；
- Constant-Time Compare；
- 不输出明文日志；
- Unit Test。

### Push 2.2：Token 创建、查看、删除

`POST /tokens`：

- 输入 `client_id`；
- 服务端生成 Token；
- 只返回一次；
- 重复 client_id 返回冲突。

`GET /tokens`：

- 只返回元数据；
- 不返回 Hash；
- 不返回明文。

`DELETE /tokens/{client_id}`：

- 标记 disabled 或删除；
- 立即阻止新 AUTH。

### Push 2.3：Token Rotate

实现：

```text
POST /tokens/{client_id}/rotate
```

要求：

- 新 Token 只返回一次；
- 旧 Token 立即失效；
- 更新时间写入；
- 并发 Rotate 有明确结果。

### Push 2.4：Admin 与 Internal Gateway 鉴权

环境变量：

```text
CONTROL_PLANE_ADMIN_TOKEN
GATEWAY_SHARED_TOKEN
```

规则：

- 管理接口使用 `Authorization: Bearer ...`；
- Gateway 内部接口使用 `X-Gateway-Token`；
- 健康检查和 `/metrics` 不使用管理 Token；
- Secret 不写入日志。

### Push 2.5：AUTH 失败限速

Redis Key：

```text
auth:failures:{identity}
```

要求：

- TTL 计数；
- 超阈值暂时拒绝；
- 成功后按策略清理；
- 指标记录失败和限速；
- 不宣称这是完整 WAF。

核心前置标签：

```text
checkpoint/token-security-before-impl
```

---

## Phase 3：Redis 状态层重构

分支：

```text
feature/p3-redis-state
```

### Push 3.1：新 Key Schema 与 Store Interface

Key：

```text
gateway:status:{gateway_id}
gateway:clients:{gateway_id}
gateway:index
token:{client_id}
token:index
config:active
auth:failures:{identity}
```

MemoryStore 继续用于单元测试，但 Docker/Kubernetes 固定使用 Redis。

### Push 3.2：Gateway Status TTL

实现：

- 状态写入；
- TTL；
- Index；
- 上报时间；
- 查询在线状态。

### Push 3.3：Client Snapshot TTL

实现：

- 每 Gateway 最新快照；
- 与 Status 使用一致过期策略；
- 不作为永久历史数据库。

### Push 3.4：Pipeline 上报

将以下操作放入 Pipeline：

- 写 Gateway 状态；
- 刷新状态 TTL；
- 更新 Gateway Index；
- 写 Client Snapshot；
- 刷新 Client TTL。

文档明确 Pipeline 不等于事务。

### Push 3.5：失效 Index 清理与 Redis 恢复

实现：

- 查询时清理状态 Key 已失效的 Gateway ID；
- Redis 启动失败不 `log.Fatalf`；
- Redis 恢复后 Ready 自动恢复；
- 真实 Redis Integration Test；
- 短暂不可用和恢复测试。

核心前置标签：

```text
checkpoint/redis-ttl-before-impl
```

---

## Phase 4：Redis Lua 配置 CAS

分支：

```text
feature/p4-config-cas
```

### Push 4.1：统一最终配置模型

删除：

```text
auth_timeout_ms
fail_open
```

新增：

```text
version
max_payload_size
max_connections_per_client
max_requests_per_client_per_second
slow_client_output_limit
log_level
request_queue_capacity_display
```

### Push 4.2：配置初始化与完整快照

- 初始版本固定；
- 完整 JSON/Hash 快照；
- 类型和范围校验；
- 旧配置迁移或明确不兼容。

### Push 4.3：Lua CAS

Lua 原子执行：

1. 读取当前版本；
2. 比较 expected version；
3. 不匹配返回 conflict；
4. 校验 new version 严格递增；
5. 写入完整配置；
6. 返回新版本。

### Push 4.4：HTTP `PUT /config`

协议：

```text
PUT /config
If-Match: <current-version>
```

结果：

- 成功：200；
- Header 缺失：428；
- 版本冲突：409；
- 非法配置：400；
- Store 错误：503 或 500，根据错误类型区分。

删除：

```text
POST /config
POST /config/reload
```

### Push 4.5：并发配置测试

必须验证：

- 20 个并发更新只有符合 CAS 的请求成功；
- 不出现新版本配旧内容；
- 冲突不会修改配置；
- Redis 错误后旧配置不变；
- C++ 只接受严格更高版本。

核心前置标签：

```text
checkpoint/config-cas-before-impl
```

---

## Phase 5：C++ 上报、动态配置与控制面客户端补齐

分支：

```text
feature/p5-cpp-telemetry
```

### Push 5.1：ControlPlaneClient 内部鉴权

增加：

```text
X-Gateway-Token
```

同时实现：

- 响应大小上限；
- 状态码解析；
- 总体超时；
- 连接、发送、接收错误分类；
- Token 不写日志。

HTTP Client 仍然是同步调用，但只在 Worker 或后台线程中运行，不进入 Reactor。

### Push 5.2：StatsSnapshot

新增一次性快照结构，包含：

```text
active_connections
total_requests
bytes_in
bytes_out
errors
request_queue_backlog
request_queue_capacity
request_queue_peak
request_queue_rejected
response_queue_backlog
response_queue_capacity
response_queue_peak
response_queue_rejected
slow_client_closed
stale_response_dropped
auth_success
auth_failure
runtime_config_version
server_state
```

避免报告线程逐项读取产生不一致快照。

### Push 5.3：补充慢客户端和过期响应指标

在明确分支递增：

- `slow_client_closed`；
- `stale_response_dropped`；
- `auth_success`；
- `auth_failure`。

新增针对性 CTest。

### Push 5.4：动态输出上限与日志等级

- 将编译期 8 MiB 固定值改为经过验证的 Runtime Config；
- 日志等级通过完整快照更新；
- 非法值保留旧配置；
- 配置回退被拒绝；
- Request Queue 容量仅展示，不运行时重构队列。

### Push 5.5：控制面故障与恢复测试

验证：

- Control Plane 不可用；
- 已认证 ECHO 继续；
- 新 AUTH 失败；
- 配置拉取保留旧快照；
- 上报失败不终止数据面；
- Control Plane 恢复后配置和上报恢复。

核心前置标签：

```text
checkpoint/cpp-metrics-before-impl
```

---

## Phase 6：Prometheus

分支：

```text
feature/p6-prometheus
```

依赖：

```text
github.com/prometheus/client_golang
```

### Push 6.1：Registry 与 `/metrics`

- 使用自建 Registry；
- 注册 Go Runtime Metrics；
- 暴露 `/metrics`；
- 指标名称、Help、Type 测试。

### Push 6.2：HTTP Middleware Metrics

指标：

```text
control_plane_http_requests_total
control_plane_http_request_duration_seconds
control_plane_http_in_flight_requests
control_plane_panics_total
```

Label 仅使用：

```text
method
route
status
```

不使用原始 URL、request_id、client_id。

### Push 6.3：Redis、AUTH 与 Config Metrics

指标：

```text
control_plane_redis_operation_duration_seconds
control_plane_redis_errors_total
control_plane_auth_total
control_plane_auth_rate_limited_total
control_plane_config_updates_total
control_plane_config_conflicts_total
```

### Push 6.4：Gateway Metrics

使用 `gateway_id` 低基数标签暴露：

```text
gateway_active_connections
gateway_requests_total
gateway_bytes_in_total
gateway_bytes_out_total
gateway_errors_total
gateway_request_queue_backlog
gateway_response_queue_backlog
gateway_request_queue_rejected_total
gateway_response_queue_rejected_total
gateway_slow_client_closed_total
gateway_stale_response_dropped_total
gateway_last_report_timestamp_seconds
gateway_online
```

### Push 6.5：过期指标清理与 Scrape 测试

- Gateway TTL 过期后 `gateway_online=0`；
- 失效 Label 可删除；
- `/metrics` 可被 Prometheus Parser 解析；
- 不出现高基数标签；
- README 提供示例查询。

核心前置标签：

```text
checkpoint/prometheus-before-impl
```

---

## Phase 7：Docker、CI 与本地故障验证

分支：

```text
feature/p7-local-integration
```

### Push 7.1：Docker 非 root 化

C++ 和 Go 镜像：

- 创建非 root 用户；
- `USER` 切换；
- 日志默认 stdout；
- 需要写文件时挂载专用目录；
- 镜像内不放 Secret。

### Push 7.2：Docker Compose 健康检查

增加：

- Redis Healthcheck；
- Control Plane live/ready；
- Gateway 启动依赖；
- Secret 使用 `.env.example`；
- `.gitignore` 排除真实 Secret。

### Push 7.3：Smoke Test v2

覆盖：

- 创建 Token；
- AUTH；
- ECHO；
- 配置 CAS；
- Gateway Status；
- Client Snapshot；
- Prometheus；
- Redis TTL；
- Token Rotate；
- Token Delete。

### Push 7.4：Redis 故障和恢复脚本

流程：

```text
正常运行
→ 暂停 Redis
→ ready 失败
→ Gateway 已认证连接继续
→ 新 AUTH 失败
→ 恢复 Redis
→ ready 恢复
→ 新 AUTH 恢复
→ Out-of-date config 不覆盖新配置
```

### Push 7.5：CI Integration Job

普通 Push/PR：

- C++ CTest；
- Sanitizers；
- Go Unit/Race/Vet；
- Redis Integration Test；
- Compose Config；
- 脚本语法。

Docker Full Smoke 可继续手动触发，也可在 PR Label 或 Nightly 运行。

---

## Phase 8：Kubernetes 与优雅摘流

分支：

```text
feature/p8-kubernetes
```

目录：

```text
deploy/kubernetes/
├── namespace.yaml
├── gateway-deployment.yaml
├── gateway-service.yaml
├── control-plane-deployment.yaml
├── control-plane-service.yaml
├── redis-statefulset.yaml
├── redis-headless-service.yaml
├── configmap.yaml
├── secret.example.yaml
├── gateway-pdb.yaml
└── control-plane-pdb.yaml
```

### Push 8.1：基础部署

- Namespace；
- Gateway 2 副本；
- Control Plane；
- Redis StatefulSet；
- PVC；
- Services；
- ConfigMap/Secret。

### Push 8.2：Resources 与 Security Context

每个工作负载增加：

- Requests/Limits；
- runAsNonRoot；
- allowPrivilegeEscalation=false；
- capabilities drop；
- seccompProfile；
- 合理只读文件系统。

### Push 8.3：Probes

Gateway：

- startupProbe：TCP 9000；
- readinessProbe：TCP 9000；
- livenessProbe：进程存活检查，不能使用会在 DRAINING 时失败并抢先重启的同一判据。

Control Plane：

- startupProbe：`/health/live`；
- readinessProbe：`/health/ready`；
- livenessProbe：`/health/live`。

### Push 8.4：preStop 与 SIGTERM

推荐 Hook：

```text
preStop
→ 向 PID 1 发送 SIGTERM
→ 等待短暂摘流传播窗口
```

要求：

- SIGTERM 幂等；
- Gateway 立即进入 DRAINING；
- 关闭 Listener；
- Readiness TCP 失败；
- 不再接受新连接和新请求；
- 已接收请求继续排空；
- 超时后强制退出。

注意：

Kubernetes 在 preStop 后还会向主进程发送终止信号，因此 C++ `stop()` 和 Signal Handler 必须支持重复触发。

约束：

```text
terminationGracePeriodSeconds
>
preStop 等待
+
Gateway Shutdown Timeout
+
安全缓冲
```

建议演示值：

```text
preStop propagation wait = 3s
Gateway shutdown timeout = 20s
terminationGracePeriodSeconds = 30s
```

这些值最终由自动化测试验证，不宣称是通用生产参数。

### Push 8.5：RollingUpdate 与 PDB

Gateway：

```text
replicas: 2
maxUnavailable: 0
maxSurge: 1
PDB minAvailable: 1
```

Control Plane 设置合理 RollingUpdate 和 PDB。

Redis 明确为单副本演示，不宣称高可用。

### Push 8.6：滚动更新自动验收

脚本：

```text
scripts/k8s_deploy.sh
scripts/k8s_smoke.sh
scripts/k8s_rolling_update_test.sh
```

测试流程：

1. 启动两个 Gateway Pod；
2. 建立持续长连接；
3. 持续发送 ECHO；
4. 触发 `rollout restart`；
5. 记录旧 Pod 进入 DRAINING；
6. 验证旧 Listener 关闭；
7. 验证旧 Pod 不再接受新连接；
8. 验证新连接进入新 Pod；
9. 验证已接收请求在截止时间内完成；
10. 验证超时连接被明确关闭；
11. 验证日志和 Prometheus 指标展示完整过程；
12. `kubectl rollout status` 成功。

核心前置标签：

```text
checkpoint/k8s-drain-before-impl
```

---

## Phase 9：性能、故障报告与发布

分支：

```text
feature/p9-release
```

### Push 9.1：Benchmark 工具完善

记录：

- 1、10、100、500 客户端；
- 不同 Payload；
- 不同 Worker；
- 不同 Queue Capacity；
- 不同慢客户端比例；
- P50/P95/P99；
- QPS；
- CPU；
- RSS；
- 队列峰值；
- 拒绝数；
- AUTH 延迟；
- Redis 延迟。

### Push 9.2：故障注入矩阵

至少覆盖：

- Control Plane 不可用；
- Redis 不可用与恢复；
- Request Queue 满；
- Response Queue 满；
- 慢客户端；
- fd 快速复用；
- 配置并发冲突；
- 非法配置；
- Gateway SIGTERM；
- Control Plane SIGTERM；
- Kubernetes Rolling Update；
- 排空中强制终止；
- Gateway 上报超时；
- Redis TTL 过期。

### Push 9.3：可复现证据

新增：

```text
results/
├── environment/
├── benchmark/
├── failures/
└── kubernetes/
```

每份报告记录：

- 机器；
- CPU；
- 内存；
- 内核；
- 编译器；
- 编译选项；
- 容器/非容器；
- Kubernetes 环境；
- Payload；
- Worker；
- 客户端模型；
- 命令；
- 原始结果；
- 限制。

### Push 9.4：README、架构图和面试文档

README 必须回答：

- 解决什么问题；
- 为什么 C++ 数据面 + Go 控制面；
- 为什么单 Reactor；
- 为什么 `fd + conn_id`；
- 为什么有界队列；
- 为什么 eventfd；
- 为什么 Redis Lua CAS；
- 为什么 Pipeline 不等于事务；
- 为什么 Prometheus 不使用高基数标签；
- Kubernetes 如何摘流；
- 系统保证与不保证；
- 如何一键验证。

### Push 9.5：Release

Release Gate：

```bash
全量 CTest
ASan/UBSan
Go Unit/Race/Vet
Redis Integration
Docker Smoke
Kubernetes Rolling Update
Benchmark Reproduction
Docs Link Check
```

发布：

```text
tag: v2.0.0
GitHub Release
release notes
```

---

# 7. 最终验收标准

项目完成必须满足：

1. 半包、粘包、连续多包、非法长度测试通过；
2. 部分写和慢客户端测试通过；
3. `fd + conn_id` 测试通过；
4. Request Queue Full 返回明确 503；
5. Response Queue Full 不静默丢响应；
6. eventfd 无固定轮询延迟；
7. Token 不以明文保存；
8. Token 创建和 Rotate 只返回一次；
9. AUTH 比较使用常量时间；
10. Redis 状态和客户端快照有 TTL；
11. Redis Pipeline 不被错误描述为原子事务；
12. Redis Lua CAS 并发更新无丢失；
13. C++ 配置只接受严格更高版本；
14. Control Plane 不可用不终止数据面；
15. Redis 不可用时 live 正常、ready 失败；
16. Redis 恢复后 ready 自动恢复；
17. `/metrics` 可被 Prometheus 正常解析；
18. 指标不使用 client_id/request_id/remote_addr 标签；
19. Go HTTP Server 支持 SIGTERM 优雅退出；
20. Gateway 支持有截止时间的 DRAINING；
21. Kubernetes Gateway 使用两个副本；
22. 旧 Pod 滚动更新时先停止接收新连接；
23. 已接收请求在截止时间内尽力排空；
24. `terminationGracePeriodSeconds` 大于完整排空预算；
25. CTest、ASan、UBSan、Go Race、CI 全部通过；
26. Docker Smoke 可复现；
27. Kubernetes Rolling Update Test 可复现；
28. 性能和故障报告包含环境、命令、原始数据和限制；
29. README 不夸大生产能力；
30. 项目 Tag `v2.0.0` 可从零部署并验收。

---

# 8. AI 开发方式

可以让 AI 完成全部代码，包括核心模块。

但执行规则固定为：

```text
一次只执行一个 Push Checkpoint
→ AI 修改
→ 运行本 Push 的测试
→ 审查 Diff
→ Commit
→ Push
→ 再进入下一 Checkpoint
```

不能让 AI：

- 一次性修改所有 Phase；
- 未跑测试就继续；
- 为解决编译错误随意删测试；
- 改变固定项目边界；
- 加入 Kafka、Gin、数据库、多 Reactor；
- 把 Secret 写入仓库；
- 在 README 声称未经验证的高并发或生产能力。

开发完成后再学习：

```text
从 checkpoint tag 创建 learn 分支
→ 保留接口、测试和需求
→ 删除核心实现
→ 自己复现
→ 与参考实现对比
```

优先复现：

- Redis Lua CAS；
- Token Hash 与 Constant-Time Compare；
- Go Middleware Chain；
- Prometheus Gateway Collector；
- C++ StatsSnapshot；
- 动态配置原子快照；
- Kubernetes preStop/SIGTERM/Readiness 排空链路。

不需要重新复现所有 CRUD、YAML 字段、日志样板和普通测试工具。

---

# 9. 最终学习主线

由于 `cpp-epoll-tcp-server` 已经掌握，Gateway 的学习重点应是增量能力：

```text
C++ 网络底座
→ eventfd / bounded queue / deadline shutdown 增量
→ C++ 与 Go 控制面的边界
→ Redis Token/TTL/Pipeline/Lua CAS
→ Go HTTP 工程化与优雅退出
→ Prometheus 指标设计
→ Kubernetes 长连接滚动更新与优雅摘流
```

面试时应能完整讲清：

```text
客户端 AUTH
→ C++ Worker 调 Go /auth/check
→ Go 查询 Redis Token Hash
→ 返回鉴权结果
→ Reactor 校验 fd + conn_id
→ 标记连接已认证
→ 请求进入有界队列
→ Worker 处理
→ eventfd 唤醒 Reactor
→ 响应回写
→ 状态周期上报
→ Prometheus 暴露
→ Kubernetes SIGTERM
→ Listener 关闭
→ Readiness 失败
→ 旧连接排空
→ 截止时间退出
```

这条链路就是最终项目的核心。
