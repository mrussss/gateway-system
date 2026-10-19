# Benchmark 方法与结果

Benchmark 用 Compose 测量已认证 TCP 请求吞吐与延迟，记录请求成功/失败、百分位延迟、AUTH 延迟、网关 STATS 前后快照；可用时另采集 Linux CPU/RSS。

## 当前完整结果

2026-10-19 对提交 f5fdfe8b4c3485e30e356a70c441ffaadd418e3c 运行完整 release gate Benchmark。环境为 WSL2 + Docker Desktop，工作区干净。4 种配置 × 9 种场景 × 3 次重复，共 36 格、108 次运行、492000 次请求；成功 492000，失败 0。

配置固定 AUTH Worker=2、AUTH Queue=32、Response Queue=4096；变化项如下：

| 配置 | Worker | Request Queue | Response Queue |
| --- | ---: | ---: | ---: |
| workers1-q64 | 1 | 64 | 4096 |
| workers1-q4096 | 1 | 4096 | 4096 |
| workers4-q64 | 4 | 64 | 4096 |
| workers4-q4096 | 4 | 4096 | 4096 |

每组包括 1/10/100/500 客户端、128/4096 字节 payload，以及 100 客户端中 10% 慢读、延迟 50ms 的场景。默认每客户端请求量依次为 1000/200/50/20。每项均通过请求记账，并记录前后 STATS 和队列容量。

容器 PID 不在 WSL /proc 命名空间，因此本次没有 CPU/RSS 样本；不据此推断进程资源使用。统计摘要见 results/benchmark/20261019-full-gate/summary.json，环境记录和方法见同目录。

## 复现

需要 Docker、curl、Python、Compose 和测试凭据。脚本默认要求干净工作区：

~~~bash
CONTROL_PLANE_ADMIN_TOKEN=... GATEWAY_SHARED_TOKEN=... TOKEN_PEPPER=... \
  scripts/benchmark_matrix.sh
~~~

默认端口被占用时，用 Compose override 和 BENCHMARK_CONTROL_PLANE_URL、BENCHMARK_GATEWAY_HOST、BENCHMARK_GATEWAY_PORT 指向隔离端口。输出目录可用 BENCHMARK_OUTPUT_DIR 改到本地 build 目录。BENCHMARK_ALLOW_DIRTY=1 只用于开发试跑，不能作为正式结果。

历史原始数据已先归档，归档位置、SHA256 和恢复方式见归档 README。仓库保留当前提交的环境记录和统计摘要。Benchmark 只代表记录的本地环境，不代表线上 SLA 或生产容量。
