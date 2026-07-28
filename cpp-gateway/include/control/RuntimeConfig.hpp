#pragma once

#include <cstdint>
#include <cstddef>
#include <string>

struct RuntimeConfig
{
    int64_t version = 1;
    int max_payload_size = 1048576;
    int max_connections_per_client = 2;
    int max_requests_per_client_per_second = 100;
    size_t slow_client_output_limit = 8 * 1024 * 1024;
    std::string log_level = "INFO";
};

bool parseRuntimeConfig(const std::string &json_body, RuntimeConfig &config);
bool applyRuntimeConfigIfNewer(RuntimeConfig &current, const RuntimeConfig &candidate);
