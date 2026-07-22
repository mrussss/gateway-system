#include "TestHarness.hpp"

#include <chrono>
#include <thread>

#include "net/TcpServer.hpp"

int main()
{
    return runTests({
        {"concurrent repeated stop is idempotent", []
         {
             TcpServer server(0, "127.0.0.1", 1, "stop-test", 16, 16, 500, 1);
             std::thread server_thread([&]
             {
                 server.start();
             });

             const auto deadline = std::chrono::steady_clock::now() +
                                   std::chrono::seconds(3);
             while (server.state() != ServerState::RUNNING &&
                    std::chrono::steady_clock::now() < deadline)
             {
                 std::this_thread::sleep_for(std::chrono::milliseconds(1));
             }
             CHECK(server.state() == ServerState::RUNNING);

             std::thread first_stop([&]
             {
                 server.stop();
             });
             std::thread second_stop([&]
             {
                 server.stop();
             });
             first_stop.join();
             second_stop.join();
             server_thread.join();
             CHECK(server.state() == ServerState::STOPPED);

             // A later call is also a no-op.
             server.stop();
             CHECK(server.state() == ServerState::STOPPED);
         }},
    });
}
