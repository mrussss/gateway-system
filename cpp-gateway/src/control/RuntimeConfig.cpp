#include "control/RuntimeConfig.hpp"

#include <exception>
#include <limits>
#include <stdexcept>

#include "nlohmann/json.hpp"
#include "protocol/ProtocolCodec.hpp"

namespace
{
int64_t integerInRange(const nlohmann::json &json, const char *field,
                       int64_t minimum, int64_t maximum)
{
    const auto &value = json.at(field);
    // get<int>() 会截断小数或窄化大整数，必须在转换前检查类型和范围。
    if (!value.is_number_integer() ||
        (value.is_number_unsigned() &&
         value.get<uint64_t>() > static_cast<uint64_t>(maximum)))
    {
        throw std::invalid_argument("invalid runtime config integer");
    }
    const int64_t number = value.get<int64_t>();
    if (number < minimum || number > maximum)
    {
        throw std::invalid_argument("runtime config integer out of range");
    }
    return number;
}
}

bool parseRuntimeConfig(const std::string &json_body, RuntimeConfig &config)
{
    try
    {
        const auto json = nlohmann::json::parse(json_body);
        RuntimeConfig parsed;
        parsed.version = integerInRange(json, "version", 1, std::numeric_limits<int64_t>::max());
        parsed.max_payload_size = static_cast<int>(
            integerInRange(json, "max_payload_size", 10, MAX_PAYLOAD_SIZE + 10));
        parsed.max_connections_per_client = static_cast<int>(
            integerInRange(json, "max_connections_per_client", 1, std::numeric_limits<int>::max()));
        parsed.max_requests_per_client_per_second =
            static_cast<int>(integerInRange(json, "max_requests_per_client_per_second",
                                           1, std::numeric_limits<int>::max()));
        parsed.slow_client_output_limit = static_cast<size_t>(
            integerInRange(json, "slow_client_output_limit", parsed.max_payload_size + 4,
                           64 * 1024 * 1024));
        parsed.log_level = json.at("log_level").get<std::string>();
        parsed.request_queue_capacity_display =
            static_cast<size_t>(integerInRange(json, "request_queue_capacity_display", 1, 65536));

        if (parsed.log_level != "DEBUG" && parsed.log_level != "INFO" &&
            parsed.log_level != "WARN" && parsed.log_level != "ERROR")
        {
            return false;
        }

        config = parsed;
        return true;
    }
    catch (const std::exception &)
    {
        return false;
    }
}

bool applyRuntimeConfigIfNewer(RuntimeConfig &current, const RuntimeConfig &candidate)
{
    if (candidate.version <= current.version)
    {
        return false;
    }
    current = candidate;
    return true;
}
