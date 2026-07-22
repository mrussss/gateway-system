#include "net/TcpServer.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <cstddef>

namespace
{
    int readIntEnv(const char *name, int default_value)
    {
        const char *value = std::getenv(name);
        if (value == nullptr)
        {
            return default_value;
        }

        try
        {
            return std::stoi(value);
        }
        catch (...)
        {
            std::cerr << "invalid integer env " << name << "=" << value
                      << ", using default " << default_value << std::endl;
            return default_value;
        }
    }

    std::string readStringEnv(const char *name, const std::string &default_value)
    {
        const char *value = std::getenv(name);
        if (value == nullptr || std::string(value).empty())
        {
            return default_value;
        }
        return value;
    }

    size_t readSizeEnv(const char *name, size_t default_value)
    {
        int value = readIntEnv(name, static_cast<int>(default_value));
        if (value <= 0)
        {
            std::cerr << "invalid positive env " << name << "=" << value
                      << ", using default " << default_value << std::endl;
            return default_value;
        }
        return static_cast<size_t>(value);
    }

    int readPositiveIntEnv(const char *name, int default_value, int maximum = 2147483647)
    {
        int value = readIntEnv(name, default_value);
        if (value <= 0 || value > maximum)
        {
            std::cerr << "invalid positive env " << name << "=" << value
                      << ", using default " << default_value << std::endl;
            return default_value;
        }
        return value;
    }

    unsigned int readWorkerCount()
    {
        int value = readIntEnv("WORKER_COUNT", 0);
        if (value < 0 || value > 1024)
        {
            std::cerr << "invalid WORKER_COUNT=" << value << ", using auto" << std::endl;
            return 0;
        }
        return static_cast<unsigned int>(value);
    }
}

int main()
{
    int gateway_port = readPositiveIntEnv("GATEWAY_PORT", 9000, 65535);
    std::string control_plane_host = readStringEnv("CONTROL_PLANE_HOST", "127.0.0.1");
    int control_plane_port = readPositiveIntEnv("CONTROL_PLANE_PORT", 8080, 65535);
    std::string gateway_id = readStringEnv("GATEWAY_ID", "gateway-001");
    size_t request_queue_capacity = readSizeEnv("REQUEST_QUEUE_CAPACITY", 4096);
    size_t response_queue_capacity = readSizeEnv("RESPONSE_QUEUE_CAPACITY", 4096);
    int shutdown_timeout_ms = readPositiveIntEnv("SHUTDOWN_TIMEOUT_MS", 5000);
    unsigned int worker_count = readWorkerCount();

    try
    {
        TcpServer server(gateway_port, control_plane_host, control_plane_port, gateway_id,
                         request_queue_capacity, response_queue_capacity,
                         shutdown_timeout_ms, worker_count);
        server.start();
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "gateway startup failed: " << error.what() << std::endl;
        return 1;
    }
}
