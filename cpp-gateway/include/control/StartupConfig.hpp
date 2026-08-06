#pragma once

#include <cstddef>
#include <string>

struct StartupConfig
{
    std::string app_environment{"production"};
    int gateway_port = 9000;
    std::string control_plane_host{"127.0.0.1"};
    int control_plane_port = 8080;
    int control_plane_timeout_ms = 1000;
    std::string gateway_id{"gateway-001"};
    std::string gateway_token;
    size_t request_queue_capacity = 4096;
    size_t response_queue_capacity = 4096;
    int shutdown_timeout_ms = 5000;
    unsigned int worker_count = 0;
    unsigned int auth_worker_count = 2;
    size_t auth_queue_capacity = 32;
};

StartupConfig parseStartupConfig();
