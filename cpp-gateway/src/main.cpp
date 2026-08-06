#include "control/StartupConfig.hpp"
#include "net/TcpServer.hpp"

#include <exception>
#include <iostream>

int main()
{
    try
    {
        const StartupConfig config = parseStartupConfig();
        TcpServer server(config.gateway_port, config.control_plane_host,
                         config.control_plane_port, config.gateway_id,
                         config.request_queue_capacity,
                         config.response_queue_capacity,
                         config.shutdown_timeout_ms, config.worker_count,
                         config.gateway_token, config.control_plane_timeout_ms,
                         config.auth_worker_count, config.auth_queue_capacity);
        server.start();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "gateway startup failed: " << error.what() << std::endl;
        return 1;
    }
}
