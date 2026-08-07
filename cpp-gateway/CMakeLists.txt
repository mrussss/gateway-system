cmake_minimum_required(VERSION 3.16)
project(gateway_system VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest)
find_package(Threads REQUIRED)

option(GATEWAY_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(GATEWAY_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

set(GATEWAY_CORE_SOURCES
    src/common/Logger.cpp
    src/net/ReactorNotifier.cpp
    src/net/TcpServer.cpp
    src/control/ControlPlaneClient.cpp
    src/control/HttpResponseParser.cpp
    src/control/SocketDeadline.cpp
    src/control/StartupConfig.cpp
    src/control/RuntimeConfig.cpp
    src/protocol/ProtocolCodec.cpp
    src/business/Dispatcher.cpp
    src/business/Handlers.cpp
    src/business/StatsManager.cpp
    src/business/LogStorage.cpp
)

add_library(gateway_core STATIC ${GATEWAY_CORE_SOURCES})
target_include_directories(gateway_core PUBLIC include)
target_compile_features(gateway_core PUBLIC cxx_std_17)
target_compile_options(gateway_core PRIVATE -Wall -Wextra -Wpedantic -Wformat=2)
target_link_libraries(gateway_core PUBLIC Threads::Threads)

if(GATEWAY_WARNINGS_AS_ERRORS)
    target_compile_options(gateway_core PRIVATE -Werror)
endif()

if(GATEWAY_ENABLE_SANITIZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(FATAL_ERROR "Sanitizers are supported only with GCC or Clang")
    endif()
    target_compile_options(gateway_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(gateway_core PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_executable(message_server src/main.cpp)
target_link_libraries(message_server PRIVATE gateway_core)
target_compile_options(message_server PRIVATE -Wall -Wextra -Wpedantic -Wformat=2)
if(GATEWAY_WARNINGS_AS_ERRORS)
    target_compile_options(message_server PRIVATE -Werror)
endif()

if(BUILD_TESTING)
    function(add_gateway_test name source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE gateway_core)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Wformat=2)
        if(GATEWAY_WARNINGS_AS_ERRORS)
            target_compile_options(${name} PRIVATE -Werror)
        endif()
        add_test(NAME ${name} COMMAND ${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 20)
    endfunction()

    add_gateway_test(protocol_codec_test tests/ProtocolCodecTest.cpp)
    add_gateway_test(block_queue_test tests/BlockQueueTest.cpp)
    add_gateway_test(runtime_config_test tests/RuntimeConfigTest.cpp)
    add_gateway_test(reactor_notifier_test tests/ReactorNotifierTest.cpp)
    add_gateway_test(server_stop_test tests/ServerStopTest.cpp)
    add_gateway_test(stats_snapshot_test tests/StatsSnapshotTest.cpp)
    add_gateway_test(control_plane_client_test tests/ControlPlaneClientTest.cpp)
    add_gateway_test(startup_config_test tests/StartupConfigTest.cpp)
    add_gateway_test(auth_task_test tests/AuthTaskTest.cpp)

    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(
        NAME graceful_shutdown_test
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/GracefulShutdownTest.py
                $<TARGET_FILE:message_server>
    )
    set_tests_properties(graceful_shutdown_test PROPERTIES TIMEOUT 30)
endif()
