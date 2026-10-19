# TCP 二进制协议

每个 TCP 帧使用大端长度前缀：

~~~text
uint32 body_length
uint8  version
uint8  message_type
uint64 request_id
bytes  payload
~~~

body_length 不含自身的 4 字节长度字段，包含其后的 version、message_type、request_id 与 payload。固定头长 10 字节；多字节整数采用网络字节序。每次读取后先校验声明长度，再等待完整消息体；因此只暂存一个不完整的尾帧。

## 消息类型

| 值 | 名称 | 说明 |
| ---: | --- | --- |
| 1 | PING | 连通性探测，响应 PONG |
| 2 | ECHO | 原样回显 payload |
| 3 | LOG_PUSH | 接收包含 level、service、message 的 JSON 日志 |
| 4 | STATS | 返回请求、错误、字节、连接和队列计数 |
| 5 | PONG | PING 响应 |
| 6 | ECHO_RESP | ECHO 响应 |
| 7 | ERROR_RESP | 业务错误响应 |
| 8 | LOG_ACK | 日志写入确认 |
| 9 | STATS_RESP | STATS 响应 |
| 10 | AUTH | 连接级认证 |
| 11 | AUTH_RESP | AUTH 结果 |

## AUTH 与连接规则

新连接必须先发送 JSON 对象，包含字符串 client_id 和 token。字段类型错误、认证被拒绝、未认证前发送业务请求、AUTH 等待期间发送第二条消息，都会关闭连接。Go/Redis 不可用时返回 AUTH_UNAVAILABLE 并失败关闭。AUTH 队列满时尽可能返回 AUTH_OVERLOADED 后关闭。成功后才允许业务请求；重复 AUTH 返回 ERROR_RESP，连接仍保持打开。

max_connections_per_client 与 max_requests_per_client_per_second 按单进程执行。达到连接上限时返回 allowed=false 并关闭；超过每秒请求限制时返回 429 错误，连接保持打开。

## 帧长度与响应

body 固定部分为 10 字节。当前 payload 上限 4 MiB，body 上限 4 MiB + 10。长度小于固定头或超出配置/硬上限时，在等待 body 前即拒绝。慢读连接超过输出缓冲限制时关闭。

解码器支持单次 read 中多个完整帧和跨 read 的分片帧。半关闭时先处理完整请求，再丢弃截断尾帧。STATS 字段的具体采集点见 cpp-gateway/src/business/Handlers.cpp 和 StatsManager.cpp。

- PING 返回 PONG；ECHO 返回原 payload。
- LOG_PUSH 成功返回 LOG_ACK；格式或写入错误返回 ERROR_RESP。
- STATS 返回队列容量、积压、峰值与拒绝计数等。
- 普通业务队列过载返回 503；AUTH 有独立过载语义。
- 非法帧和被拒绝的 AUTH 关闭连接。
