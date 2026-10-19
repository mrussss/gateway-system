# 错误处理

- 不完整帧：保留已收数据并等待后续读取。
- 长度非法：记录错误并关闭连接。
- Request/AUTH Queue 满：明确拒绝；AUTH 尝试响应后关闭。
- Queue 停止：拒绝新工作，保留已入队工作供退出排空。
- Response Queue 满或停止：通知 Reactor，只关闭代次匹配的连接。
- 输出超限：关闭慢读连接。
- 迟到响应：conn_id 不匹配时丢弃。
- EPOLLERR/HUP/RDHUP：统一走连接关闭逻辑。
- 配置获取或解析失败：保留当前完整快照。
- AUTH 控制面不可用：失败关闭。
- 退出 deadline 到期：关闭余下连接和未完成工作。

BlockQueue 的 FULL、STOPPED、stop 和 deadline 后 abort 语义以源码和测试为准。跨组件故障见[故障语义](../../docs/failure_semantics.md)。
