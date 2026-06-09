# 自定义 TCP 协议说明

## 1. 协议概述

本项目使用自定义二进制协议进行通信。协议采用长度前缀（length-prefixed）方式编码，每个消息包由固定长度的头部和变长的消息体组成。

协议设计原则：
- **解析高效**：二进制编码，无需文本解析
- **自描述**：每个包包含长度信息，天然支持分片重组
- **校验友好**：包长限制、版本检查和类型校验易于实现

---

## 2. 包结构（Packet Format）

### 2.1 整体布局

```
| 4 字节 body_length | 1 字节 version | 1 字节 type | 8 字节 request_id | 变长 payload |
├─ Header (4 bytes) ─┤├─────────── Body (body_length bytes) ──────────────┤
```

### 2.2 字段说明

| 偏移 | 长度 | 字段 | 字节序 | 说明 |
|---|---|---|---|---|
| 0 | 4 | body_length | 网络序（大端） | 整个 Body 的长度，即 version + type + request_id + payload 的总字节数 |
| 4 | 1 | version | 大端 | 协议版本号，当前固定为 1 |
| 5 | 1 | type | 大端 | 消息类型，见 MessageType 定义 |
| 6 | 8 | request_id | 网络序（大端） | 请求 ID，用于请求与响应关联 |
| 14 | 变长 | payload | — | 消息负载，UTF-8 编码的 JSON 字符串或任意数据 |

### 2.3 常量定义

```
FIXED_BODY_SIZE = 1 + 1 + 8 = 10    // version + type + request_id
MIN_BODY_SIZE   = 10                 // 最小 Body 长度（不带 payload）
MAX_PAYLOAD_SIZE = 4 * 1024 * 1024   // 4 MB，最大 Payload 大小
MAX_BODY_SIZE   = MAX_PAYLOAD_SIZE + FIXED_BODY_SIZE
```

---

## 3. 消息类型（MessageType）

| 值 | 请求类型 | 说明 | 对应响应类型 |
|---|---|---|---|
| 1 | `PING` | 连通性探测 | `PONG` (5) |
| 2 | `ECHO` | 回显，服务端原样返回 payload | `ECHO_RESP` (6) |
| 3 | `LOG_PUSH` | 日志上报，JSON 格式 | `LOG_ACK` (8) |
| 4 | `STATS` | 查询服务器监控统计 | `STATS_RESP` (9) |
| 10 | `AUTH` | 连接级认证，认证通过后才允许业务请求 | `AUTH_RESP` (11) |
| 其他 | — | 未识别的消息类型 | `ERROR_RESP` (7) |

响应类型：

| 值 | 响应类型 | 说明 |
|---|---|---|
| 5 | `PONG` | PING 的响应 |
| 6 | `ECHO_RESP` | ECHO 的响应，payload 为原始请求的 payload |
| 7 | `ERROR_RESP` | 错误响应，payload 为 JSON 格式的错误描述 |
| 8 | `LOG_ACK` | LOG_PUSH 的确认响应 |
| 9 | `STATS_RESP` | STATS 的响应，payload 为 JSON 格式的统计信息 |
| 11 | `AUTH_RESP` | AUTH 的响应，payload 为 JSON 格式的认证结果 |

---

## 4. 请求与响应格式

### 4.1 AUTH / AUTH_RESP

客户端连接 C++ 网关后，必须先发送 AUTH 包。网关收到 AUTH 后调用 Go 控制面的 `POST /auth/check`。认证通过后连接进入已认证状态，后续才允许发送 PING/ECHO/LOG_PUSH/STATS。

```
请求:  body_length=10+N, version=1, type=10 (AUTH), request_id=<id>,
       payload={"client_id":"client_001","token":"test-token"}
响应:  body_length=10+N, version=1, type=11 (AUTH_RESP), request_id=<id>,
       payload={"allowed":true,"reason":"ok"}
```

如果客户端未认证就发送业务请求，或 AUTH payload 非法/认证失败，网关会关闭连接。

### 4.2 PING / PONG

```
请求:  body_length=10, version=1, type=1 (PING), request_id=<id>,  payload=空
响应:  body_length=10, version=1, type=5 (PONG), request_id=<id>,  payload=空
```

### 4.3 ECHO / ECHO_RESP

```
请求:  body_length=10+N, version=1, type=2 (ECHO), request_id=<id>,  payload=<任意数据>
响应:  body_length=10+N, version=1, type=6 (ECHO_RESP), request_id=<id>,  payload=<与请求相同的 payload>
```

### 4.4 LOG_PUSH / LOG_ACK

```
请求:  body_length=10+N, version=1, type=3 (LOG_PUSH), request_id=<id>,
       payload={"level":"INFO","service":"auth-service","message":"user login success"}
```

LOG_PUSH 的 payload 必须是合法 JSON 对象，且必须包含以下三个字符串字段：
- `level`：日志级别
- `service`：服务名称
- `message`：日志内容

校验通过后写入本地日志文件，返回：

```
响应:  body_length=39, version=1, type=8 (LOG_ACK), request_id=<id>,
       payload={"status":"success"}
```

### 4.5 STATS / STATS_RESP

```
请求:  body_length=10, version=1, type=4 (STATS), request_id=<id>,  payload=空
响应:  payload={"total_requests":1024,"total_logs":512,...}
```

STATS_RESP 的 payload 为 JSON 格式，包含以下统计字段：

| 字段 | 类型 | 说明 |
|---|---|---|
| total_requests | uint64 | 服务启动后总请求数 |
| total_logs | uint64 | 成功写入的日志数 |
| total_errors | uint64 | 错误数 |
| total_recv_bytes | uint64 | 接收字节总数 |
| total_sent_bytes | uint64 | 发送字节总数 |
| active_connections | uint64 | 当前活跃连接数 |
| total_request_queue_backlog | uint64 | 请求队列积压数 |
| total_response_queue_backlog | uint64 | 响应队列积压数 |

### 4.6 ERROR_RESP

当请求无法处理时，服务端返回 ERROR_RESP（type=7），payload 为固定 JSON 格式：

```json
{"status":<状态码>,"message":"<错误描述>"}
```

常见的错误响应：

| 场景 | payload |
|---|---|
| 未识别的消息类型 | `{"status":400,"message":"unknown type"}` |
| payload 为空 | `{"status":400,"message":"payload is empty"}` |
| payload 过大 | `{"status":400,"message":"payload too large"}` |
| JSON 格式错误 | `{"status":400,"message":"invalid json"}` |
| JSON 字段校验失败 | `{"status":400,"message":"invalid log format"}` |
| 日志写入失败 | `{"status":500,"message":"log write failed."}` |

---

## 5. 粘包与半包处理

### 5.1 半包处理

TCP 是流式协议，一个应用层包可能被分片多次到达。解码器维护游标 `read_index` 和状态标记 `need_more_data`：

```
步骤 1: 检查剩余数据是否 >= 4 字节
         否 → 设置 need_more_data=true, 跳出循环
步骤 2: 读取 body_length
步骤 3: 检查 body_length 是否在 [FIXED_BODY_SIZE, MAX_BODY_SIZE] 范围内
         否 → 返回 INVALID_LENGTH, 关闭连接
步骤 4: 检查剩余数据是否 >= 4 + body_length
         否 → 设置 need_more_data=true, 跳出循环
步骤 5: 解析完整包, read_index 前进 (4 + body_length) 字节
步骤 6: 回到步骤 1, 检查是否还有更多完整包
步骤 7: 处理完所有完整包后, erase 已处理数据, 返回 NEED_MORE_DATA 或 OK
```

### 5.2 粘包处理

当一次 recv 包含多个完整包时，解码器通过 while 循环连续解析，每次解析后 `read_index` 前移 `4 + body_length` 字节，直到剩余数据不足以构成一个完整包为止。

例如，一次 recv 收到 3 个完整包的数据量：

```
| 包1 header(4) | 包1 body(N1) | 包2 header(4) | 包2 body(N2) | 包3 header(4) | 包3 body(N3) |
```

解码器会依次解析出包1、包2、包3，将所有 Request 加入 `out_requests` 数组，最后更新 `input_buffer`。

---

## 6. 协议校验与安全防护

### 6.1 长度校验

解码器在解析包体之前检查 `body_length`：

```cpp
constexpr uint32_t FIXED_BODY_SIZE = 1 + 1 + 8;  // = 10
constexpr uint32_t MAX_BODY_SIZE = MAX_PAYLOAD_SIZE + FIXED_BODY_SIZE;

if (host_body_length < FIXED_BODY_SIZE || host_body_length > MAX_BODY_SIZE)
{
    return DecodeStatus::INVALID_LENGTH;
}
```

这防止了两种极端情况：
- `body_length` 小于最小包头长度（10），避免无符号整数下溢
- `body_length` 超过最大允许值（4MB + 10），防止恶意超大包导致 OOM

### 6.2 版本检查

当前协议版本为 1。版本号保留用于未来协议升级，实现向后兼容的版本协商。

### 6.3 JSON 校验

LOG_PUSH 的 payload 使用 nlohmann/json 严格校验：

```cpp
auto j = nlohmann::json::parse(request.payload);
if (!j.is_object() ||
    !j.contains("level") || !j["level"].is_string() ||
    !j.contains("service") || !j["service"].is_string() ||
    !j.contains("message") || !j["message"].is_string())
{
    return makeErrorResponse(request, 400, "invalid log format");
}
```

非法 JSON 字符串会触发 `parse_error` 异常，捕获后返回 `400 invalid json`。

---

## 7. 编码格式

响应编码与请求解码使用相同的包结构：

```cpp
uint32_t body_length = 1 + 1 + 8 + response.payload.size();
uint32_t net_body_len = htonl(body_length);
uint64_t net_req_id = htobe64(response.request_id);

// 按顺序写入: 4字节长度 + 1字节版本 + 1字节类型 + 8字节ID + 变长payload
result.append(reinterpret_cast<const char*>(&net_body_len), 4);
result.append(reinterpret_cast<const char*>(&version), 1);
result.append(reinterpret_cast<const char*>(&msg_type), 1);
result.append(reinterpret_cast<const char*>(&net_req_id), 8);
result.append(response.payload);
```

所有多字节整数均使用网络字节序（大端）传输，确保跨平台兼容性。
