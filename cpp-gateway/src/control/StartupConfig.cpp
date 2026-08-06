#include "control/StartupConfig.hpp"

#include <charconv>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>

namespace
{
constexpr uint64_t SHUTDOWN_SAFETY_MARGIN_MS = 100;

std::string readString(const char *name, std::string default_value, bool allow_empty)
{
    const char *raw = std::getenv(name);
    if (raw == nullptr)
    {
        return default_value;
    }
    std::string value(raw);
    if (!allow_empty && value.empty())
    {
        throw std::invalid_argument(std::string(name) + " must not be empty");
    }
    return value;
}

uint64_t readUnsigned(const char *name, uint64_t default_value, uint64_t minimum,
                      uint64_t maximum)
{
    const char *raw = std::getenv(name);
    if (raw == nullptr)
    {
        return default_value;
    }

    const std::string_view text(raw);
    uint64_t value = 0;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value < minimum || value > maximum)
    {
        throw std::invalid_argument(std::string(name) + " must be an integer in [" +
                                    std::to_string(minimum) + ", " +
                                    std::to_string(maximum) + "]");
    }
    return value;
}
} // namespace

StartupConfig parseStartupConfig()
{
    StartupConfig config;
    config.gateway_port = static_cast<int>(
        readUnsigned("GATEWAY_PORT", config.gateway_port, 1, 65535));
    config.control_plane_host = readString("CONTROL_PLANE_HOST",
                                           config.control_plane_host, false);
    config.control_plane_port = static_cast<int>(
        readUnsigned("CONTROL_PLANE_PORT", config.control_plane_port, 1, 65535));
    config.control_plane_timeout_ms = static_cast<int>(
        readUnsigned("CONTROL_PLANE_TIMEOUT_MS", config.control_plane_timeout_ms,
                     100, 30000));
    config.gateway_id = readString("GATEWAY_ID", config.gateway_id, false);
    config.gateway_token = readString("GATEWAY_SHARED_TOKEN", "", true);
    config.request_queue_capacity = static_cast<size_t>(
        readUnsigned("REQUEST_QUEUE_CAPACITY", config.request_queue_capacity, 1, 65536));
    config.response_queue_capacity = static_cast<size_t>(
        readUnsigned("RESPONSE_QUEUE_CAPACITY", config.response_queue_capacity, 1, 65536));
    config.shutdown_timeout_ms = static_cast<int>(
        readUnsigned("SHUTDOWN_TIMEOUT_MS", config.shutdown_timeout_ms, 1, 300000));
    config.worker_count = static_cast<unsigned int>(
        readUnsigned("WORKER_COUNT", config.worker_count, 0, 1024));
    config.auth_worker_count = static_cast<unsigned int>(
        readUnsigned("AUTH_WORKER_COUNT", config.auth_worker_count, 1, 16));
    config.auth_queue_capacity = static_cast<size_t>(
        readUnsigned("AUTH_QUEUE_CAPACITY", config.auth_queue_capacity, 1, 65536));
    const uint64_t minimum_shutdown_ms =
        2ULL * static_cast<uint64_t>(config.control_plane_timeout_ms) +
        SHUTDOWN_SAFETY_MARGIN_MS;
    if (static_cast<uint64_t>(config.shutdown_timeout_ms) < minimum_shutdown_ms)
    {
        throw std::invalid_argument(
            "SHUTDOWN_TIMEOUT_MS must be at least 2 * CONTROL_PLANE_TIMEOUT_MS + " +
            std::to_string(SHUTDOWN_SAFETY_MARGIN_MS));
    }
    return config;
}
