# 连接生命周期

Reactor 独占连接状态、Socket I/O、epoll interest 和关闭操作。Worker 只处理值对象；响应必须同时匹配 fd 和单调递增的 conn_id。

~~~text
accept4(NONBLOCK | CLOEXEC)
  → 分配 conn_id
  → 注册 EPOLLIN | EPOLLET | EPOLLRDHUP
  → AUTH_PENDING
  → AUTHENTICATED
  → 业务请求与输出
  → 对端关闭、错误、策略拒绝或服务退出
  → epoll DEL + close + erase
~~~

只有成功进入 Request 或 AUTH 有界队列的工作才增加 in_flight_work。Reactor 确认 fd 和 conn_id 仍对应同一连接后，才因响应减少计数。本地过载或校验响应不计入。

## 对端半关闭

收到 EPOLLRDHUP 或 recv 返回 0 后移除读事件；缓存完整帧仍会接纳，残缺尾帧记录后丢弃。已接纳响应仍可写。in_flight_work 为 0 且输出缓冲为空后才关闭连接。

先处理致命 EPOLLERR，再排空可读数据，之后标记 read_eof 并刷新输出，避免 FIN 丢弃完整 AUTH、粘连请求或完整帧后的截断尾帧。

## 服务退出

DRAINING 停止 accept 和新业务解码，已接纳工作和响应继续排空至 deadline。队列拒绝记录也携带完整 fd + conn_id，不能误关复用后的新连接。详见[退出契约](shutdown.md)。
