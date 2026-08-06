#pragma once

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "control/RuntimeConfig.hpp"

enum class HttpError
{
    None,
    ResolveFailed,
    DeadlineExceeded,
    ConnectFailed,
    SendFailed,
    ReceiveFailed,
    HeaderTooLarge,
    BodyTooLarge,
    MalformedResponse,
    MissingContentLength,
    DuplicateContentLength,
    UnsupportedTransferEncoding,
    PrematureEof,
    HttpStatusError,
    InvalidJson,
};

const char *httpErrorCategory(HttpError error) noexcept;

struct HttpResult
{
    HttpError error = HttpError::None;
    int status_code = 0;
    std::string body;

    bool ok() const noexcept
    {
        return error == HttpError::None && status_code >= 200 && status_code < 300;
    }
};

enum class AuthOutcome
{
    Allowed,
    Denied,
    Unavailable,
};

struct AuthResult
{
    AuthOutcome outcome = AuthOutcome::Unavailable;
    HttpError http_error = HttpError::None;
    int http_status = 0;
    std::string reason_code;
};

struct ControlPlaneMetricsSnapshot
{
    uint64_t requests_auth{};
    uint64_t requests_config{};
    uint64_t requests_metrics_report{};
    uint64_t requests_clients_report{};
    uint64_t duration_total_us{};
    uint64_t errors_resolve{};
    uint64_t errors_deadline{};
    uint64_t errors_connect{};
    uint64_t errors_send{};
    uint64_t errors_receive{};
    uint64_t errors_protocol{};
    uint64_t errors_status{};
    uint64_t errors_json{};
    uint64_t errors_oversize{};
};

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
    uint64_t auth_queue_capacity;
    uint64_t auth_queue_backlog;
    uint64_t auth_queue_peak;
    uint64_t auth_queue_rejected;
    uint64_t auth_in_flight;
    uint64_t auth_tasks_cancelled_before_start;
    uint64_t response_queue_capacity;
    uint64_t response_queue_backlog;
    uint64_t response_queue_peak;
    uint64_t response_queue_rejected;
    uint64_t response_queue_rejected_normal;
    uint64_t response_queue_rejected_auth;
    uint64_t slow_client_closed;
    uint64_t stale_response_dropped;
    uint64_t auth_success;
    uint64_t auth_failure;
    uint64_t auth_allowed;
    uint64_t auth_denied;
    uint64_t auth_unavailable;
    uint64_t auth_duration_count;
    uint64_t auth_duration_total_us;
    ControlPlaneMetricsSnapshot control_plane;
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
    static constexpr size_t MAX_HTTP_HEADER_BYTES = 16 * 1024;
    static constexpr size_t MAX_HTTP_BODY_BYTES = 1024 * 1024;

    ControlPlaneClient(std::string host, int port, int timeout_ms,
                       std::string gateway_token = "");

    AuthResult checkAuth(const std::string &client_id, const std::string &token) const;
    bool fetchConfig(RuntimeConfig &config) const;
    bool reportMetrics(const GatewayMetrics &metrics) const;
    bool reportClients(const std::string &gateway_id,
                       const std::vector<ClientReport> &clients) const;
    ControlPlaneMetricsSnapshot metricsSnapshot() const noexcept;

private:
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;

    HttpResult requestJson(std::string_view method, std::string_view path,
                           std::string_view body) const;
    HttpResult requestJsonOnce(std::string_view method, std::string_view path,
                               std::string_view body) const;
    HttpResult readHttpResponseWithDeadline(int fd, Deadline deadline) const;
    HttpError sendAllWithDeadline(int fd, std::string_view data, Deadline deadline) const;

    std::string hostHeader() const;
    static void validateHeaderValue(std::string_view name, std::string_view value);
    void recordRequestMetrics(std::string_view path, const HttpResult &result,
                              uint64_t duration_us) const noexcept;

    std::string host_;
    int port_;
    int timeout_ms_;
    std::string gateway_token_;
    mutable std::atomic<uint64_t> requests_auth_{0};
    mutable std::atomic<uint64_t> requests_config_{0};
    mutable std::atomic<uint64_t> requests_metrics_report_{0};
    mutable std::atomic<uint64_t> requests_clients_report_{0};
    mutable std::atomic<uint64_t> duration_total_us_{0};
    mutable std::atomic<uint64_t> errors_resolve_{0};
    mutable std::atomic<uint64_t> errors_deadline_{0};
    mutable std::atomic<uint64_t> errors_connect_{0};
    mutable std::atomic<uint64_t> errors_send_{0};
    mutable std::atomic<uint64_t> errors_receive_{0};
    mutable std::atomic<uint64_t> errors_protocol_{0};
    mutable std::atomic<uint64_t> errors_status_{0};
    mutable std::atomic<uint64_t> errors_json_{0};
    mutable std::atomic<uint64_t> errors_oversize_{0};
};
