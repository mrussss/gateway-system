# 测试与验证

测试保护项目范围内的协议、队列、连接所有权、控制面故障处理及退出行为。

## C++

CTest 覆盖 framing、有界队列、eventfd、运行配置解析与版本、AUTH task、控制面 HTTP parser/deadline、fd + conn_id、半关闭和 graceful shutdown。Python 黑盒测试覆盖单帧/粘包/AUTH 半关闭、EOF 截断、并发退出、AUTH 隔离、队列过载、fd 复用、慢读、故障恢复和 deadline 退出。

~~~bash
cmake -S cpp-gateway -B cpp-gateway/build -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build cpp-gateway/build --parallel
ctest --test-dir cpp-gateway/build --output-on-failure
~~~

## Go

Go 测试覆盖 Token 摘要与生命周期、generation CAS、AUTH 失败 TTL/原子性/fail-closed、配置 CAS、HTTP 校验、readiness 和 context/deadline 退出。

~~~bash
cd go-control-plane
go test ./...
go test -race ./...
go vet ./...
~~~

## Redis 与 Compose

以 REDIS_TEST_ADDR 指向隔离 Redis，真实集成覆盖 Token 生命周期、AUTH 失败计数与 TTL、Lua CAS 冲突及故障语义。Full Gate 还运行 Compose Token/AUTH/配置/TCP Smoke、Redis pause/recovery 和认证 TCP Benchmark。

运行 scripts/release_gate.sh --fast 或 --full。Full Gate 会清理当前 Compose 项目及其卷；执行前核对项目名、端口和数据卷。CI、单独 Smoke、Benchmark 与 Full Gate 是不同验证，不能相互替代。
