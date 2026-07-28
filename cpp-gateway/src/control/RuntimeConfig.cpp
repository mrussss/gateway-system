#include "control/RuntimeConfig.hpp"

#include <exception>

#include "nlohmann/json.hpp"
#include "protocol/ProtocolCodec.hpp"

bool parseRuntimeConfig(const std::string &json_body, RuntimeConfig &config)
{
    try
    {
        const auto json = nlohmann::json::parse(json_body);
        RuntimeConfig parsed;
        parsed.version = json.at("version").get<int64_t>();
        parsed.max_payload_size = json.at("max_payload_size").get<int>();
        parsed.max_connections_per_client = json.at("max_connections_per_client").get<int>();
        parsed.max_requests_per_client_per_second =
            json.at("max_requests_per_client_per_second").get<int>();
        parsed.slow_client_output_limit = json.at("slow_client_output_limit").get<size_t>();
        parsed.log_level = json.at("log_level").get<std::string>();

        if (parsed.version <= 0 || parsed.max_payload_size < 10 ||
            parsed.max_payload_size > static_cast<int>(MAX_PAYLOAD_SIZE + 10) ||
            parsed.max_connections_per_client <= 0 ||
            parsed.max_requests_per_client_per_second <= 0 ||
            parsed.slow_client_output_limit < static_cast<size_t>(parsed.max_payload_size) ||
            parsed.slow_client_output_limit > 64 * 1024 * 1024 ||
            (parsed.log_level != "DEBUG" && parsed.log_level != "INFO" &&
             parsed.log_level != "WARN" && parsed.log_level != "ERROR"))
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
