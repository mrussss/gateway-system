# 冻结范围

当前版本已具备 CTest、严格编译、Sanitizer、有界队列、eventfd、epoll 错误处理、偏移输出缓冲、安全日志和有期限退出。

网络架构保持单 Reactor；不计划为追逐新技术加入多 Reactor、lock-free 队列、coroutine、Kafka、TLS 或替换 HTTP 框架。后续维护限于 Bug、安全问题、测试和面试反馈。
