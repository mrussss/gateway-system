# C++ TCP 网关

C++17 数据面运行在 Linux，采用单 Reactor、非阻塞 Socket、epoll ET 和 length-prefixed 协议。Normal 与 AUTH 分队列执行；Worker 通过有界 Response Queue 和 eventfd 把结果交回 Reactor。

## 构建与测试

~~~bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug -DGATEWAY_WARNINGS_AS_ERRORS=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
~~~

ASan/UBSan：

~~~bash
cmake -S . -B build-sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DGATEWAY_WARNINGS_AS_ERRORS=ON -DGATEWAY_ENABLE_SANITIZERS=ON
cmake --build build-sanitized --parallel
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
ctest --test-dir build-sanitized --output-on-failure
~~~

## 核心不变量

- Reactor 独占 accept、read、write、close 和 epoll interest 修改。
- 工作成功入队后才计入 in-flight；响应必须匹配相同 fd + conn_id。
- AUTH Worker 才调用控制面；控制面延迟不占用普通 Worker。
- ET 读取持续到 EAGAIN；写入偏移保留尚未发送的数据。
- 半关闭时先处理完整帧，再等已接纳工作和输出排空。
- 队列拒绝、停止、背压和 DRAINING 都有显式处理；退出有 deadline。

参数和限制见根目录[项目说明](../README.md)，测试见[测试说明](../docs/testing.md)。
