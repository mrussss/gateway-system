#include "net/TcpServer.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

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
}

int main()
{
    int gateway_port = readIntEnv("GATEWAY_PORT", 9000);
    std::string control_plane_host = readStringEnv("CONTROL_PLANE_HOST", "127.0.0.1");
    int control_plane_port = readIntEnv("CONTROL_PLANE_PORT", 8080);
    std::string gateway_id = readStringEnv("GATEWAY_ID", "gateway-001");

    TcpServer server(gateway_port, control_plane_host, control_plane_port, gateway_id);

    server.start();
    return 0;
}
