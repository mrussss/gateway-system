# C++ 网关性能路径

Worker 成功推送 Response 后写入 eventfd，阻塞的 Reactor 随即从 epoll_wait 返回并排空响应；正常运行不使用固定周期轮询。eventfd 也用于 stop 通知和 Response Queue 严重拒绝处理。

输出通过 output_buffer + write_offset 保留未发送字节，避免部分写后反复搬移缓冲。需要追加时才压缩已发送区间。

本地结果与方法见[Benchmark 说明](../../docs/benchmark.md)。机器、Docker 网络和线程调度会影响数据；不代表生产容量。

已知限制包括每个新连接的同步 HTTP AUTH、固定大小的独立 AUTH Worker、LOG_PUSH 同步写文件、单 Reactor 和 Python Benchmark client。
