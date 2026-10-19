# 连接生命周期

accept4 创建非阻塞、CLOEXEC Socket 后分配递增 conn_id，再注册 EPOLLIN、EPOLLET、EPOLLRDHUP。连接经过 AUTH_PENDING、AUTHENTICATED，最终由 Reactor 删除 epoll interest 并关闭。

异步响应必须同时匹配 fd 与 conn_id。ET 读取持续到 EAGAIN；部分写推进 write_offset，遇到 EAGAIN 后等待 EPOLLOUT。半关闭时先处理可读帧，等已接纳工作和输出排空后才关闭。

DRAINING 停止监听与新解码，已接纳工作继续排空至 deadline。详见[连接生命周期契约](../../docs/connection_lifecycle.md)。
