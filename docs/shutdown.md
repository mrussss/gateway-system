# 有期限的优雅退出

## 状态机

~~~text
RUNNING
  │ SIGINT / SIGTERM / stop()
  │ eventfd 唤醒
  ▼
DRAINING
  │ 已接纳工作与输出排空
  │ 或 SHUTDOWN_TIMEOUT_MS 到期
  ▼
STOPPED
~~~

## C++ 网关

1. stop 请求写入 eventfd，使阻塞中的 epoll_wait 返回。
2. Reactor 幂等地进入 DRAINING，关闭监听 Socket 并移除客户端 EPOLLIN。
3. Request Queue 和 AUTH Queue 停止接收新任务；Worker 继续处理已接纳任务。
4. Worker 发布最后响应并通知 eventfd。两组 Worker 退出后停止 Response Queue 并再次通知。
5. Reactor 排空响应并发送输出缓冲。in_flight_work 为 0 且输出为空后连接才关闭。
6. deadline 先到时，中止尚未开始的工作和响应并关闭剩余连接。

## 保证与边界

Worker 会继续派发已入队请求；若退出 deadline 到期，尚未开始的工作可能被中止。响应会尝试写入内核，直到完成或 deadline 到期。慢读客户端不能单独延长退出时间。启动校验要求 SHUTDOWN_TIMEOUT_MS >= 2 * CONTROL_PLANE_TIMEOUT_MS + 100。

deadline 到期可截断响应，也可丢弃尚未开始的已接纳工作。Socket deadline 限制非阻塞 connect/send/receive；同步 getaddrinfo 无法被它中断，是明确的 DNS 退出边界。

## Go 控制面

signal.NotifyContext 取消根 context。HTTP Server 在有限 deadline 内 Shutdown，然后关闭 Store。listener 关闭后拒绝新请求，在途请求可以在期限内完成。

graceful_shutdown_test 覆盖重复 stop、普通与 AUTH 积压、并行 ECHO、任务取消、fd 复用、慢读输出、控制面故障和 deadline 强制退出。
