# C++ 控制面客户端

ControlPlaneClient 只支持数据面所需的两个 HTTP 调用：专用 AUTH Worker 请求 POST /auth/check，后台线程请求 GET /config。

HttpResponseParser 增量解析并限制 header/body 大小，要求有效 Content-Length，不支持 Transfer-Encoding。SocketDeadline 为非阻塞 connect、send、receive 共用一个绝对 deadline。同步 getaddrinfo 不受该 deadline 中断，是明确记录的边界。

AUTH 的 HTTP、JSON 或后端错误均失败关闭；配置请求错误或版本无效时保留 last-known-good。Metrics report、在线客户端报告及 Fleet API 不属于当前范围。
