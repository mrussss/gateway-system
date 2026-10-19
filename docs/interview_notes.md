# 面试交流速记

## 一句话介绍

这是一个 Linux C++ 鉴权长连接网关，重点在 Reactor 连接所有权、有界并发、过载处理、依赖隔离、版本配置及有期限退出。

## 常见问题

- 为什么一个 Reactor 独占 Socket？让 epoll、关闭和输出缓冲只有一个修改者，简化生命周期。
- 为什么队列有界？过载必须明确反馈，不能承诺无限内存或无限等待。
- 为什么 AUTH 单独排队？Go/Redis 延迟不应占满普通 TCP Worker。
- 为什么响应携带 fd 和 conn_id？内核可能在断连后复用整数 fd。
- eventfd 有什么作用？Worker 完成后唤醒阻塞的 epoll，不用周期轮询。
- 部分写如何处理？保留 write offset，启用 EPOLLOUT 后继续发送。
- Redis 故障有什么影响？readiness 失败、新 AUTH 失败关闭；已认证连接的普通请求仍可继续。
- 配置如何避免并发覆盖？Go 校验并用 Redis Lua CAS；C++ 校验后只切换较新不可变快照。
- 退出如何有界？RUNNING → DRAINING，停止监听和新业务，尽量排空已接纳工作，超时关闭剩余连接。

## 验证证据

可引用 CTest、ASan/UBSan、Go race/vet、真实 Redis、Compose Smoke/恢复和 Benchmark。说明覆盖条件与限制，不把本地性能数值描述为线上 SLA。项目不声称提供 Kubernetes、Prometheus 产品监控、多网关管理、客户端汇总或 Redis 高可用。
