# Full Gate Benchmark：2026-10-19

- 被测提交：f5fdfe8b4c3485e30e356a70c441ffaadd418e3c。
- 环境：WSL2 + Docker Desktop；已在公开记录中隐去主机名。
- 完整 Release Gate：通过。
- 4 种 Worker/Request Queue 配置、9 个场景、每组重复 3 次：36 格、108 次运行。
- 默认请求量：1/10/100/500 客户端各 1000/200/50/20 次；共 492000 次，成功 492000、失败 0。
- 每次运行记录请求记账、客户端结果、网关 STATS 前后快照和队列容量。
- Docker 容器 PID 不在 WSL /proc 中，CPU/RSS 样本为空；不据此报告进程资源用量。
- 统计摘要：summary.json；环境记录：environment.txt。

108 份原始 JSON 在本机归档和被忽略的 cpp-gateway/build/a-completion/benchmark/ 中。校验方法及 SHA256 见本机归档 README。结果只反映本地测试环境，不代表线上 SLA 或生产容量。
