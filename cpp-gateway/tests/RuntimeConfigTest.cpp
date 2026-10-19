#include "TestHarness.hpp"

#include <string>
#include <utility>
#include <vector>

#include "control/ControlPlaneClient.hpp"
#include "control/RuntimeConfig.hpp"
#include "nlohmann/json.hpp"

int main()
{
    const std::string valid = R"({
        "version": 2,
        "max_payload_size": 1024,
        "max_connections_per_client": 4,
        "max_requests_per_client_per_second": 200,
        "slow_client_output_limit": 8192,
        "log_level": "DEBUG",
        "request_queue_capacity_display": 4096
    })";

    return runTests({
        {"valid config parses atomically", [&]
         {
             RuntimeConfig config;
             CHECK(parseRuntimeConfig(valid, config));
             CHECK_EQ(config.version, int64_t{2});
             CHECK_EQ(config.max_payload_size, 1024);
             CHECK_EQ(config.max_connections_per_client, 4);
             CHECK_EQ(config.request_queue_capacity_display, size_t{4096});
         }},
        {"invalid config does not mutate destination", []
         {
             RuntimeConfig config;
             config.version = 7;
             config.max_payload_size = 777;
             CHECK(!parseRuntimeConfig(R"({"version":8,"max_payload_size":0})", config));
             CHECK_EQ(config.version, int64_t{7});
             CHECK_EQ(config.max_payload_size, 777);
         }},
        {"numeric fields reject fractions and narrowing overflow", [&]
         {
             const std::vector<std::pair<std::string, nlohmann::json>> invalid = {
                 {"version", 2.5},
                 {"version", uint64_t{9223372036854775808ULL}},
                 {"max_payload_size", 1024.5},
                 {"max_payload_size", 9},
                 {"max_connections_per_client", uint64_t{4294967297ULL}},
                 {"max_requests_per_client_per_second", uint64_t{4294967297ULL}},
                 {"slow_client_output_limit", 8192.5},
                 {"slow_client_output_limit", -1},
                 {"request_queue_capacity_display", 4096.5},
                 {"request_queue_capacity_display", 65537},
             };
             for (const auto &[field, value] : invalid)
             {
                 auto json = nlohmann::json::parse(valid);
                 json[field] = value;
                 RuntimeConfig config;
                 config.version = 7;
                 CHECK(!parseRuntimeConfig(json.dump(), config));
                 CHECK_EQ(config.version, int64_t{7});
             }
         }},
        {"maximum integer limits remain exact", [&]
         {
             auto json = nlohmann::json::parse(valid);
             json["version"] = int64_t{9223372036854775807LL};
             json["max_connections_per_client"] = 2147483647;
             json["max_requests_per_client_per_second"] = 2147483647;
             RuntimeConfig config;
             CHECK(parseRuntimeConfig(json.dump(), config));
             CHECK_EQ(config.version, int64_t{9223372036854775807LL});
             CHECK_EQ(config.max_connections_per_client, 2147483647);
             CHECK_EQ(config.max_requests_per_client_per_second, 2147483647);
         }},
        {"output limit includes the frame length prefix", [&]
         {
             auto json = nlohmann::json::parse(valid);
             json["slow_client_output_limit"] = 1027;
             RuntimeConfig config;
             CHECK(!parseRuntimeConfig(json.dump(), config));
             json["slow_client_output_limit"] = 1028;
             CHECK(parseRuntimeConfig(json.dump(), config));
         }},
        {"config above compile-time protocol limit is rejected", []
         {
             RuntimeConfig config;
             config.version = 3;
             const std::string oversized = R"({
                 "version": 4,
                 "max_payload_size": 4194315,
                 "max_connections_per_client": 2,
                 "max_requests_per_client_per_second": 100,
                 "slow_client_output_limit": 8388608,
                 "log_level": "INFO",
                 "request_queue_capacity_display": 4096
             })";
             CHECK(!parseRuntimeConfig(oversized, config));
             CHECK_EQ(config.version, int64_t{3});
         }},
        {"older and equal versions do not overwrite", []
         {
             RuntimeConfig current;
             current.version = 5;
             current.max_payload_size = 500;
             RuntimeConfig candidate;
             candidate.version = 4;
             candidate.max_payload_size = 400;
             CHECK(!applyRuntimeConfigIfNewer(current, candidate));
             CHECK_EQ(current.max_payload_size, 500);
             candidate.version = 5;
             CHECK(!applyRuntimeConfigIfNewer(current, candidate));
             CHECK_EQ(current.max_payload_size, 500);
         }},
        {"newer version replaces the whole snapshot", []
         {
             RuntimeConfig current;
             RuntimeConfig candidate;
             candidate.version = 2;
             candidate.slow_client_output_limit = 4096;
             candidate.log_level = "DEBUG";
             candidate.request_queue_capacity_display = 32;
             CHECK(applyRuntimeConfigIfNewer(current, candidate));
             CHECK_EQ(current.version, int64_t{2});
             CHECK_EQ(current.slow_client_output_limit, size_t{4096});
             CHECK_EQ(current.log_level, std::string{"DEBUG"});
             CHECK_EQ(current.request_queue_capacity_display, size_t{32});
         }},
        {"fetch failure retains caller config", []
         {
             RuntimeConfig config;
             config.version = 9;
             config.max_payload_size = 999;
             ControlPlaneClient unavailable("127.0.0.1", 1, 25);
             CHECK(!unavailable.fetchConfig(config));
             CHECK_EQ(config.version, int64_t{9});
             CHECK_EQ(config.max_payload_size, 999);
         }},
    });
}
