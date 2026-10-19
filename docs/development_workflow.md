# 开发与验证流程

## 定向检查

Go 改动运行 gofmt、单测、race 和 vet；C++ 改动运行相应 CTest，涉及内存边界或生命周期时运行 Sanitizer。先做与修改匹配的检查，再运行更大的 gate。

## Release Gate

scripts/release_gate.sh --fast 覆盖 C++ 构建/CTest、ASan/UBSan、Go 测试/Race/Vet、Compose 配置、Shell/Python 语法与 Markdown 链接检查。

scripts/release_gate.sh --full 再运行真实 Redis、Compose Smoke、Redis 中断/恢复和完整 TCP Benchmark。Docker 或隔离 Redis 不可用时，准确记为未运行，不能算通过。

Full Gate、Smoke、Redis recovery 和 Benchmark 会清理自己的 Compose 项目及其卷。运行前核对项目名、服务、端口和卷，只用可丢弃数据。Benchmark 默认要求工作区干净；BENCHMARK_ALLOW_DIRTY=1 只供开发试跑，不作为正式证据。

## 范围控制

保持 Reactor 对 Socket 生命周期的独占、有界队列语义和 last-known-good 配置。改动限于 Bug、安全问题、测试或面试反馈；新增产品能力须先明确范围。
