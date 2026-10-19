# C++ 数据面架构

~~~text
epoll Reactor
  ├─ listener 和 client descriptor
  ├─ Connection map（fd + conn_id）
  ├─ 协议解析与本地策略
  ├─ 有界 Request Queue → Worker
  ├─ eventfd ◄── 有界 Response Queue
  └─ 带 offset 的输出缓冲
~~~

Reactor 修改连接状态；普通 Worker 执行业务，独立 AUTH Executor 调用控制面。结果经有界 Response Queue 返回，并以 eventfd 唤醒 epoll。

完整所有权与数据流见[系统架构](../../docs/architecture.md)。
