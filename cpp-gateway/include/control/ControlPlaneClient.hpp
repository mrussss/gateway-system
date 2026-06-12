#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct GatewayMetrics
{
    std::string gateway_id;
    uint64_t active_connections;
    uint64_t total_messages;
    uint64_t bytes_in;
    uint64_t bytes_out;
    uint64_t error_count;
    int64_t timestamp;
};

struct ClientReport
{
    std::string client_id;
    std::string remote_addr;
    std::string connected_at;
};

struct RuntimeConfig
{
    int64_t version = 1;
    int auth_timeout_ms = 1000;
    int max_payload_size = 4194314;
    int max_connections_per_client = 2;
    int max_requests_per_client_per_second = 100;
    bool fail_open = false;
};

class ControlPlaneClient
{
public:
    ControlPlaneClient(std::string host, int port, int timeout_ms);

    bool checkAuth(const std::string &client_id, const std::string &token) const;
    bool fetchConfig(RuntimeConfig &config) const;
    bool reportMetrics(const GatewayMetrics &metrics) const;
    bool reportClients(const std::string &gateway_id, const std::vector<ClientReport> &clients) const;

private:
    bool getJson(const std::string &path, std::string &response_body) const;
    bool postJson(const std::string &path, const std::string &body, std::string &response_body) const;
    int connectWithTimeout() const;
    bool sendAll(int fd, const std::string &data) const;
    std::string readResponse(int fd) const;

    std::string host_;
    int port_;
    int timeout_ms_;
};
