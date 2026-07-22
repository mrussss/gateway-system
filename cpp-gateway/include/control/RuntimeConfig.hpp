#pragma once

#include <cstdint>
#include <string>

struct RuntimeConfig
{
    int64_t version = 1;
    int auth_timeout_ms = 1000;
    int max_payload_size = 4194314;
    int max_connections_per_client = 2;
    int max_requests_per_client_per_second = 100;
    bool fail_open = false;
};

bool parseRuntimeConfig(const std::string &json_body, RuntimeConfig &config);
bool applyRuntimeConfigIfNewer(RuntimeConfig &current, const RuntimeConfig &candidate);
