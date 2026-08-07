#include "control/ControlPlaneClient.hpp"

#include <chrono>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

#include "common/Logger.hpp"
#include "control/HttpResponseParser.hpp"
#include "control/SocketDeadline.hpp"
#include "nlohmann/json.hpp"

namespace
{
void logHttpFailure(const char *operation, const HttpResult &result)
{
    LOG_ERROR("control plane request failed: operation=%s category=%s status=%d",
              operation, httpErrorCategory(result.error), result.status_code);
}
} // namespace

const char *httpErrorCategory(HttpError error) noexcept
{
    switch (error)
    {
    case HttpError::None: return "none";
    case HttpError::ResolveFailed: return "resolve";
    case HttpError::DeadlineExceeded: return "deadline";
    case HttpError::ConnectFailed: return "connect";
    case HttpError::SendFailed: return "send";
    case HttpError::ReceiveFailed: return "receive";
    case HttpError::HeaderTooLarge:
    case HttpError::BodyTooLarge: return "oversize";
    case HttpError::HttpStatusError: return "http_status";
    case HttpError::InvalidJson: return "json";
    case HttpError::MalformedResponse:
    case HttpError::MissingContentLength:
    case HttpError::DuplicateContentLength:
    case HttpError::UnsupportedTransferEncoding:
    case HttpError::PrematureEof: return "protocol";
    }
    return "unknown";
}

ControlPlaneClient::ControlPlaneClient(std::string host, int port, int timeout_ms,
                                       std::string gateway_token)
    : host_(std::move(host)), port_(port), timeout_ms_(timeout_ms),
      gateway_token_(std::move(gateway_token))
{
    if (host_.empty())
    {
        throw std::invalid_argument("control plane host must not be empty");
    }
    if (port_ < 1 || port_ > 65535)
    {
        throw std::invalid_argument("control plane port must be in [1, 65535]");
    }
    if (timeout_ms_ <= 0)
    {
        throw std::invalid_argument("control plane timeout must be positive");
    }
    validateHeaderValue("control plane host", host_);
    validateHeaderValue("gateway token", gateway_token_);
}

AuthResult ControlPlaneClient::checkAuth(const std::string &client_id,
                                         const std::string &token,
                                         std::optional<Deadline> not_after) const
{
    const nlohmann::json payload = {
        {"client_id", client_id},
        {"token", token},
    };
    HttpResult response = requestJson("POST", "/auth/check", payload.dump(), not_after);

    AuthResult result;
    result.http_error = response.error;
    result.http_status = response.status_code;
    if (!response.ok())
    {
        logHttpFailure("auth", response);
        return result;
    }

    try
    {
        const auto body = nlohmann::json::parse(response.body);
        if (!body.is_object() || !body.contains("allowed") ||
            !body["allowed"].is_boolean())
        {
            result.http_error = HttpError::InvalidJson;
            errors_json_.fetch_add(1, std::memory_order_relaxed);
            return result;
        }

        if (body.contains("code") && body["code"].is_string())
        {
            result.reason_code = body["code"].get<std::string>();
        }
        else if (body.contains("reason") && body["reason"].is_string())
        {
            result.reason_code = body["reason"].get<std::string>();
        }
        result.outcome = body["allowed"].get<bool>() ? AuthOutcome::Allowed
                                                     : AuthOutcome::Denied;
        return result;
    }
    catch (const std::exception &error)
    {
        LOG_ERROR("control plane auth response parse failed: category=json error=%s",
                  error.what());
        result.http_error = HttpError::InvalidJson;
        errors_json_.fetch_add(1, std::memory_order_relaxed);
        return result;
    }
}

bool ControlPlaneClient::fetchConfig(RuntimeConfig &config) const
{
    HttpResult response = requestJson("GET", "/config", "");
    if (!response.ok())
    {
        logHttpFailure("config", response);
        return false;
    }

    RuntimeConfig parsed;
    if (!parseRuntimeConfig(response.body, parsed))
    {
        response.error = HttpError::InvalidJson;
        errors_json_.fetch_add(1, std::memory_order_relaxed);
        logHttpFailure("config", response);
        return false;
    }
    config = parsed;
    return true;
}

bool ControlPlaneClient::reportMetrics(const GatewayMetrics &metrics) const
{
    const nlohmann::json payload = {
        {"gateway_id", metrics.gateway_id},
        {"gateway_boot_id", metrics.gateway_boot_id},
        {"process_start_time", metrics.process_start_time},
        {"active_connections", metrics.active_connections},
        {"total_requests", metrics.total_requests},
        {"bytes_in", metrics.bytes_in},
        {"bytes_out", metrics.bytes_out},
        {"error_count", metrics.error_count},
        {"request_queue_capacity", metrics.request_queue_capacity},
        {"request_queue_backlog", metrics.request_queue_backlog},
        {"request_queue_peak", metrics.request_queue_peak},
        {"request_queue_rejected", metrics.request_queue_rejected},
        {"auth_queue_capacity", metrics.auth_queue_capacity},
        {"auth_queue_backlog", metrics.auth_queue_backlog},
        {"auth_queue_peak", metrics.auth_queue_peak},
        {"auth_queue_rejected", metrics.auth_queue_rejected},
        {"auth_in_flight", metrics.auth_in_flight},
        {"auth_tasks_cancelled_before_start", metrics.auth_tasks_cancelled_before_start},
        {"response_queue_capacity", metrics.response_queue_capacity},
        {"response_queue_backlog", metrics.response_queue_backlog},
        {"response_queue_peak", metrics.response_queue_peak},
        {"response_queue_rejected", metrics.response_queue_rejected},
        {"response_queue_rejected_normal", metrics.response_queue_rejected_normal},
        {"response_queue_rejected_auth", metrics.response_queue_rejected_auth},
        {"slow_client_closed", metrics.slow_client_closed},
        {"stale_response_dropped", metrics.stale_response_dropped},
        {"auth_success", metrics.auth_success},
        {"auth_failure", metrics.auth_failure},
        {"auth_allowed", metrics.auth_allowed},
        {"auth_denied", metrics.auth_denied},
        {"auth_unavailable", metrics.auth_unavailable},
        {"auth_duration_count", metrics.auth_duration_count},
        {"auth_duration_total_us", metrics.auth_duration_total_us},
        {"control_plane_requests_auth", metrics.control_plane.requests_auth},
        {"control_plane_requests_config", metrics.control_plane.requests_config},
        {"control_plane_requests_metrics_report", metrics.control_plane.requests_metrics_report},
        {"control_plane_requests_clients_report", metrics.control_plane.requests_clients_report},
        {"control_plane_duration_total_us", metrics.control_plane.duration_total_us},
        {"control_plane_errors_resolve", metrics.control_plane.errors_resolve},
        {"control_plane_errors_deadline", metrics.control_plane.errors_deadline},
        {"control_plane_errors_connect", metrics.control_plane.errors_connect},
        {"control_plane_errors_send", metrics.control_plane.errors_send},
        {"control_plane_errors_receive", metrics.control_plane.errors_receive},
        {"control_plane_errors_protocol", metrics.control_plane.errors_protocol},
        {"control_plane_errors_status", metrics.control_plane.errors_status},
        {"control_plane_errors_json", metrics.control_plane.errors_json},
        {"control_plane_errors_oversize", metrics.control_plane.errors_oversize},
        {"runtime_config_version", metrics.runtime_config_version},
        {"server_state", metrics.server_state},
        {"timestamp", metrics.timestamp},
    };

    HttpResult response = requestJson("POST", "/metrics/report", payload.dump());
    if (!response.ok())
    {
        logHttpFailure("metrics_report", response);
        return false;
    }
    return true;
}

bool ControlPlaneClient::reportClients(
    const std::string &gateway_id, const std::vector<ClientReport> &clients) const
{
    nlohmann::json client_items = nlohmann::json::array();
    for (const auto &client : clients)
    {
        client_items.push_back({
            {"client_id", client.client_id},
            {"remote_addr", client.remote_addr},
            {"connected_at", client.connected_at},
        });
    }

    const nlohmann::json payload = {
        {"gateway_id", gateway_id},
        {"clients", client_items},
    };
    HttpResult response = requestJson("POST", "/clients/report", payload.dump());
    if (!response.ok())
    {
        logHttpFailure("clients_report", response);
        return false;
    }
    return true;
}

HttpResult ControlPlaneClient::requestJson(std::string_view method, std::string_view path,
                                           std::string_view body,
                                           std::optional<Deadline> not_after) const
{
    const auto started_at = Clock::now();
    HttpResult result = requestJsonOnce(method, path, body, not_after);
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - started_at);
    recordRequestMetrics(path, result, static_cast<uint64_t>(duration.count()));
    return result;
}

HttpResult ControlPlaneClient::requestJsonOnce(std::string_view method,
                                               std::string_view path,
                                               std::string_view body,
                                               std::optional<Deadline> not_after) const
{
    validateHeaderValue("HTTP method", method);
    validateHeaderValue("HTTP path", path);
    if (path.empty() || path.front() != '/' ||
        (method != "GET" && method != "POST"))
    {
        return {HttpError::MalformedResponse, 0, {}};
    }

    HttpError connect_error = HttpError::None;
    auto socket = control_detail::SocketDeadline::connect(
        host_, port_, timeout_ms_, not_after, connect_error);
    if (!socket)
    {
        return {connect_error, 0, {}};
    }

    std::ostringstream request;
    request << method << ' ' << path << " HTTP/1.1\r\n"
            << "Host: " << hostHeader() << "\r\n"
            << "Accept: application/json\r\n"
            << "X-Gateway-Token: " << gateway_token_ << "\r\n";
    if (method == "POST")
    {
        request << "Content-Type: application/json\r\n"
                << "Content-Length: " << body.size() << "\r\n";
    }
    request << "Connection: close\r\n\r\n";
    if (method == "POST")
    {
        request << body;
    }

    const std::string serialized = request.str();
    const HttpError sent = socket->sendAll(serialized);
    if (sent != HttpError::None)
    {
        return {sent, 0, {}};
    }

    control_detail::HttpResponseParser parser;
    char buffer[4096];
    while (!parser.done())
    {
        const control_detail::SocketReadResult received =
            socket->receive(buffer, sizeof(buffer));
        if (received.error != HttpError::None)
        {
            parser.finishOnTransportError(received.error);
            break;
        }
        if (received.eof)
        {
            parser.finishOnEof();
            break;
        }
        parser.consume(std::string_view(buffer, received.size));
    }
    return parser.takeResult();
}

std::string ControlPlaneClient::hostHeader() const
{
    std::string host = host_;
    if (host.find(':') != std::string::npos &&
        !(host.size() >= 2 && host.front() == '[' && host.back() == ']'))
    {
        host = '[' + host + ']';
    }
    return host + ':' + std::to_string(port_);
}

void ControlPlaneClient::validateHeaderValue(std::string_view name,
                                             std::string_view value)
{
    if (value.find('\r') != std::string_view::npos ||
        value.find('\n') != std::string_view::npos)
    {
        throw std::invalid_argument(std::string(name) + " must not contain CR or LF");
    }
}

void ControlPlaneClient::recordRequestMetrics(std::string_view path,
                                              const HttpResult &result,
                                              uint64_t duration_us) const noexcept
{
    if (path == "/auth/check") { requests_auth_.fetch_add(1, std::memory_order_relaxed); }
    else if (path == "/config") { requests_config_.fetch_add(1, std::memory_order_relaxed); }
    else if (path == "/metrics/report") { requests_metrics_report_.fetch_add(1, std::memory_order_relaxed); }
    else if (path == "/clients/report") { requests_clients_report_.fetch_add(1, std::memory_order_relaxed); }
    duration_total_us_.fetch_add(duration_us, std::memory_order_relaxed);

    switch (result.error)
    {
    case HttpError::None: break;
    case HttpError::ResolveFailed: errors_resolve_.fetch_add(1, std::memory_order_relaxed); break;
    case HttpError::DeadlineExceeded: errors_deadline_.fetch_add(1, std::memory_order_relaxed); break;
    case HttpError::ConnectFailed: errors_connect_.fetch_add(1, std::memory_order_relaxed); break;
    case HttpError::SendFailed: errors_send_.fetch_add(1, std::memory_order_relaxed); break;
    case HttpError::ReceiveFailed: errors_receive_.fetch_add(1, std::memory_order_relaxed); break;
    case HttpError::HttpStatusError: errors_status_.fetch_add(1, std::memory_order_relaxed); break;
    case HttpError::InvalidJson: errors_json_.fetch_add(1, std::memory_order_relaxed); break;
    case HttpError::HeaderTooLarge:
    case HttpError::BodyTooLarge:
        errors_oversize_.fetch_add(1, std::memory_order_relaxed);
        break;
    case HttpError::MalformedResponse:
    case HttpError::MissingContentLength:
    case HttpError::DuplicateContentLength:
    case HttpError::UnsupportedTransferEncoding:
    case HttpError::PrematureEof:
        errors_protocol_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

ControlPlaneMetricsSnapshot ControlPlaneClient::metricsSnapshot() const noexcept
{
    ControlPlaneMetricsSnapshot result;
    result.requests_auth = requests_auth_.load(std::memory_order_relaxed);
    result.requests_config = requests_config_.load(std::memory_order_relaxed);
    result.requests_metrics_report = requests_metrics_report_.load(std::memory_order_relaxed);
    result.requests_clients_report = requests_clients_report_.load(std::memory_order_relaxed);
    result.duration_total_us = duration_total_us_.load(std::memory_order_relaxed);
    result.errors_resolve = errors_resolve_.load(std::memory_order_relaxed);
    result.errors_deadline = errors_deadline_.load(std::memory_order_relaxed);
    result.errors_connect = errors_connect_.load(std::memory_order_relaxed);
    result.errors_send = errors_send_.load(std::memory_order_relaxed);
    result.errors_receive = errors_receive_.load(std::memory_order_relaxed);
    result.errors_protocol = errors_protocol_.load(std::memory_order_relaxed);
    result.errors_status = errors_status_.load(std::memory_order_relaxed);
    result.errors_json = errors_json_.load(std::memory_order_relaxed);
    result.errors_oversize = errors_oversize_.load(std::memory_order_relaxed);
    return result;
}
