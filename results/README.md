# 测试证据

本目录只保留当前项目范围需要的结果摘要与方法。C++ 构建、Sanitizer、Go、真实 Redis、Compose Smoke/恢复和 Benchmark 命令见[测试说明](../docs/testing.md)。

历史 Benchmark 原始 JSON 已先行归档；修改前的 206 个跟踪文件可由本机归档包或 Git 提交 f5fdfe8 恢复。当前摘要见 benchmark/20261019-full-gate/。

Benchmark 脚本默认在 results/benchmark/<run-id>/ 生成原始输出；新 JSON 不纳入 Git，仅保留有复核价值的 summary.json 和方法说明。不要提交凭据、本地 .env 或过程日志。
