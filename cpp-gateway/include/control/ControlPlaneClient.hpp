#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "control/RuntimeConfig.hpp"

struct GatewayMetrics
{
    std::string gateway_id;
    std::string gateway_boot_id;
    int64_t process_start_time;
    uint64_t active_connections;
    uint64_t total_requests;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t error_count;
    uint64_t request_queue_capacity;
    uint64_t request_queue_backlog;
    uint64_t request_queue_peak;
    uint64_t request_queue_rejected;
    uint64_t response_queue_capacity;
    uint64_t response_queue_backlog;
    uint64_t response_queue_peak;
    uint64_t response_queue_rejected;
    uint64_t slow_client_closed;
    uint64_t stale_response_dropped;
    uint64_t auth_success;
    uint64_t auth_failure;
    int64_t runtime_config_version;
    std::string server_state;
    int64_t timestamp;
};

struct ClientReport
{
    std::string client_id;
    std::string remote_addr;
    std::string connected_at;
};

class ControlPlaneClient
{
public:
    ControlPlaneClient(std::string host, int port, int timeout_ms,
                       std::string gateway_token = "");

    bool checkAuth(const std::string &client_id, const std::string &token) const;
    bool fetchConfig(RuntimeConfig &config) const;
    bool reportMetrics(const GatewayMetrics &metrics) const;
    bool reportClients(const std::string &gateway_id, const std::vector<ClientReport> &clients) const;

private:
    bool getJson(const std::string &path, std::string &response_body) const;
    bool postJson(const std::string &path, const std::string &body, std::string &response_body) const;
    int connectWithTimeout() const;
    bool sendAll(int fd, const std::string &data) const;
    bool readResponse(int fd, std::string &response) const;

    std::string host_;
    int port_;
    int timeout_ms_;
    std::string gateway_token_;
    static constexpr size_t max_response_bytes_ = 1024 * 1024;
};
