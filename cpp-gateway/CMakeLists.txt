# 1. 声明最低要求的 CMake 版本和项目名称
cmake_minimum_required(VERSION 3.10)
project(MessageServer)

# 2. 查找系统多线程库
find_package(Threads REQUIRED)

# 3. 指定头文件搜索路径
# 这样在 cpp 文件中就可以直接 #include "net/TcpServer.hpp" 
# 而不用写繁琐的 "../../include/net/TcpServer.hpp" 了
include_directories(include)

# 4. 定义要编译的源文件列表
set(SRC_FILES
    src/main.cpp
    src/net/TcpServer.cpp
    src/control/ControlPlaneClient.cpp
    src/protocol/ProtocolCodec.cpp
    src/business/Dispatcher.cpp
    src/business/Handlers.cpp
    src/business/StatsManager.cpp
    src/business/LogStorage.cpp
)

# 5. 生成可执行文件
add_executable(message_server ${SRC_FILES})

# 6. 设置现代 C++ 编译选项
target_compile_features(message_server PRIVATE cxx_std_17)
target_compile_options(message_server PRIVATE -Wall -Wextra -O2)

# 7. 链接跨平台线程库
target_link_libraries(message_server PRIVATE Threads::Threads)
