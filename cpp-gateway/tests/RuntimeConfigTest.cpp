#include "TestHarness.hpp"

#include <string>

#include "control/ControlPlaneClient.hpp"
#include "control/RuntimeConfig.hpp"

int main()
{
    const std::string valid = R"({
        "version": 2,
        "auth_timeout_ms": 500,
        "max_payload_size": 1024,
        "max_connections_per_client": 4,
        "max_requests_per_client_per_second": 200,
        "fail_open": false
    })";

    return runTests({
        {"valid config parses atomically", [&]
         {
             RuntimeConfig config;
             CHECK(parseRuntimeConfig(valid, config));
             CHECK_EQ(config.version, int64_t{2});
             CHECK_EQ(config.max_payload_size, 1024);
             CHECK_EQ(config.max_connections_per_client, 4);
         }},
        {"invalid config does not mutate destination", []
         {
             RuntimeConfig config;
             config.version = 7;
             config.max_payload_size = 777;
             CHECK(!parseRuntimeConfig(R"({"version":8,"auth_timeout_ms":0})", config));
             CHECK_EQ(config.version, int64_t{7});
             CHECK_EQ(config.max_payload_size, 777);
         }},
        {"config above compile-time protocol limit is rejected", []
         {
             RuntimeConfig config;
             config.version = 3;
             const std::string oversized = R"({
                 "version": 4,
                 "auth_timeout_ms": 1000,
                 "max_payload_size": 4194315,
                 "max_connections_per_client": 2,
                 "max_requests_per_client_per_second": 100,
                 "fail_open": false
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
             candidate.auth_timeout_ms = 222;
             candidate.fail_open = true;
             CHECK(applyRuntimeConfigIfNewer(current, candidate));
             CHECK_EQ(current.version, int64_t{2});
             CHECK_EQ(current.auth_timeout_ms, 222);
             CHECK(current.fail_open);
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
