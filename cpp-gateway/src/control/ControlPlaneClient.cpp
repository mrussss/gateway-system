#include "control/ControlPlaneClient.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cctype>
#include <cstring>
#include <memory>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "common/Logger.hpp"
#include "nlohmann/json.hpp"

namespace
{
using Clock = std::chrono::steady_clock;
using Deadline = Clock::time_point;

class UniqueFd
{
public:
    explicit UniqueFd(int fd = -1) noexcept : fd_(fd) {}
    ~UniqueFd()
    {
        if (fd_ != -1)
        {
            close(fd_);
        }
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    UniqueFd(UniqueFd &&other) noexcept : fd_(other.release()) {}
    UniqueFd &operator=(UniqueFd &&other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ != -1; }
    int release() noexcept
    {
        const int value = fd_;
        fd_ = -1;
        return value;
    }
    void reset(int fd = -1) noexcept
    {
        if (fd_ != -1)
        {
            close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_;
};

struct AddrInfoDeleter
{
    void operator()(addrinfo *value) const noexcept
    {
        if (value != nullptr)
        {
            freeaddrinfo(value);
        }
    }
};

std::optional<int> remainingMilliseconds(Deadline deadline)
{
    const auto now = Clock::now();
    if (deadline <= now)
    {
        return std::nullopt;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::min<int64_t>(remaining.count(), INT_MAX));
}

HttpError waitForSocket(int fd, short requested_events, Deadline deadline,
                        HttpError failure)
{
    while (true)
    {
        const auto timeout = remainingMilliseconds(deadline);
        if (!timeout)
        {
            return HttpError::DeadlineExceeded;
        }

        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = requested_events;
        const int result = poll(&descriptor, 1, *timeout);
        if (result > 0)
        {
            if ((descriptor.revents & POLLNVAL) != 0)
            {
                return failure;
            }
            if ((descriptor.revents &
                 (requested_events | POLLERR | POLLHUP)) != 0)
            {
                // The subsequent connect/send/recv obtains the precise result.
                return HttpError::None;
            }
            continue;
        }
        if (result == 0)
        {
            return HttpError::DeadlineExceeded;
        }
        if (errno != EINTR)
        {
            return failure;
        }
    }
}

std::string trimAsciiWhitespace(std::string_view value)
{
    size_t first = 0;
    while (first < value.size() &&
           (value[first] == ' ' || value[first] == '\t'))
    {
        ++first;
    }
    size_t last = value.size();
    while (last > first && (value[last - 1] == ' ' || value[last - 1] == '\t'))
    {
        --last;
    }
    return std::string(value.substr(first, last - first));
}

std::string lowerAscii(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character)
    {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

bool parseContentLength(std::string_view raw_value, size_t &value)
{
    const std::string text = trimAsciiWhitespace(raw_value);
    if (text.empty())
    {
        return false;
    }

    size_t parsed = 0;
    for (const unsigned char character : text)
    {
        if (character < '0' || character > '9')
        {
            return false;
        }
        const size_t digit = static_cast<size_t>(character - '0');
        if (parsed > (ControlPlaneClient::MAX_HTTP_BODY_BYTES - digit) / 10)
        {
            value = ControlPlaneClient::MAX_HTTP_BODY_BYTES + 1;
            return true;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

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
                                         const std::string &token) const
{
    const nlohmann::json payload = {
        {"client_id", client_id},
        {"token", token},
    };
    HttpResult response = requestJson("POST", "/auth/check", payload.dump());

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
                                           std::string_view body) const
{
    const auto started_at = Clock::now();
    HttpResult result = requestJsonOnce(method, path, body);
    const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
        Clock::now() - started_at);
    recordRequestMetrics(path, result, static_cast<uint64_t>(duration.count()));
    return result;
}

HttpResult ControlPlaneClient::requestJsonOnce(std::string_view method,
                                               std::string_view path,
                                               std::string_view body) const
{
    validateHeaderValue("HTTP method", method);
    validateHeaderValue("HTTP path", path);
    if (path.empty() || path.front() != '/' ||
        (method != "GET" && method != "POST"))
    {
        return {HttpError::MalformedResponse, 0, {}};
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *raw_addresses = nullptr;
    const std::string port = std::to_string(port_);
    const int resolve_result = getaddrinfo(host_.c_str(), port.c_str(), &hints,
                                           &raw_addresses);
    if (resolve_result != 0)
    {
        LOG_ERROR("control plane request failed: category=resolve host=%s port=%d error=%s",
                  host_.c_str(), port_, gai_strerror(resolve_result));
        return {HttpError::ResolveFailed, 0, {}};
    }
    std::unique_ptr<addrinfo, AddrInfoDeleter> addresses(raw_addresses);

    // Synchronous getaddrinfo is the documented boundary. All socket operations and
    // all resolved addresses share this one deadline after resolution completes.
    const Deadline deadline = Clock::now() + std::chrono::milliseconds(timeout_ms_);
    UniqueFd socket_fd;
    HttpError connect_error = HttpError::ConnectFailed;
    for (addrinfo *address = addresses.get(); address != nullptr; address = address->ai_next)
    {
        if (!remainingMilliseconds(deadline))
        {
            return {HttpError::DeadlineExceeded, 0, {}};
        }

        UniqueFd candidate(socket(address->ai_family,
                                  address->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                  address->ai_protocol));
        if (!candidate)
        {
            continue;
        }

        int result = connect(candidate.get(), address->ai_addr, address->ai_addrlen);
        if (result == -1 && errno != EINPROGRESS && errno != EINTR)
        {
            continue;
        }
        if (result == -1)
        {
            const HttpError waited = waitForSocket(candidate.get(), POLLOUT, deadline,
                                                   HttpError::ConnectFailed);
            if (waited == HttpError::DeadlineExceeded)
            {
                return {waited, 0, {}};
            }
            if (waited != HttpError::None)
            {
                continue;
            }

            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                           &socket_error_size) == -1 || socket_error != 0)
            {
                continue;
            }
        }
        socket_fd = std::move(candidate);
        connect_error = HttpError::None;
        break;
    }
    if (!socket_fd)
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
    const HttpError sent = sendAllWithDeadline(socket_fd.get(), serialized, deadline);
    if (sent != HttpError::None)
    {
        return {sent, 0, {}};
    }
    return readHttpResponseWithDeadline(socket_fd.get(), deadline);
}

HttpError ControlPlaneClient::sendAllWithDeadline(int fd, std::string_view data,
                                                   Deadline deadline) const
{
    size_t offset = 0;
    while (offset < data.size())
    {
        if (!remainingMilliseconds(deadline))
        {
            return HttpError::DeadlineExceeded;
        }
        const ssize_t sent = send(fd, data.data() + offset, data.size() - offset,
                                  MSG_NOSIGNAL);
        if (sent > 0)
        {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent == -1 && errno == EINTR)
        {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            const HttpError waited = waitForSocket(fd, POLLOUT, deadline,
                                                   HttpError::SendFailed);
            if (waited != HttpError::None)
            {
                return waited;
            }
            continue;
        }
        return HttpError::SendFailed;
    }
    return HttpError::None;
}

HttpResult ControlPlaneClient::readHttpResponseWithDeadline(int fd,
                                                            Deadline deadline) const
{
    std::string received;
    received.reserve(MAX_HTTP_HEADER_BYTES);
    size_t header_end = std::string::npos;
    char buffer[4096];

    auto receiveMore = [&](HttpError eof_error) -> HttpError
    {
        while (true)
        {
            if (!remainingMilliseconds(deadline))
            {
                return HttpError::DeadlineExceeded;
            }
            const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
            if (count > 0)
            {
                received.append(buffer, static_cast<size_t>(count));
                return HttpError::None;
            }
            if (count == 0)
            {
                return eof_error;
            }
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                const HttpError waited = waitForSocket(fd, POLLIN, deadline,
                                                       HttpError::ReceiveFailed);
                if (waited != HttpError::None)
                {
                    return waited;
                }
                continue;
            }
            return HttpError::ReceiveFailed;
        }
    };

    while ((header_end = received.find("\r\n\r\n")) == std::string::npos)
    {
        if (received.size() > MAX_HTTP_HEADER_BYTES)
        {
            return {HttpError::HeaderTooLarge, 0, {}};
        }
        const HttpError result = receiveMore(HttpError::MalformedResponse);
        if (result != HttpError::None)
        {
            return {result, 0, {}};
        }
    }
    if (header_end + 4 > MAX_HTTP_HEADER_BYTES)
    {
        return {HttpError::HeaderTooLarge, 0, {}};
    }

    const size_t status_end = received.find("\r\n");
    if (status_end == std::string::npos || status_end >= header_end)
    {
        return {HttpError::MalformedResponse, 0, {}};
    }

    const std::string_view status_line(received.data(), status_end);
    if (status_line.size() < 12 ||
        (status_line.substr(0, 8) != "HTTP/1.0" &&
         status_line.substr(0, 8) != "HTTP/1.1") ||
        status_line[8] != ' ' ||
        status_line[9] < '1' || status_line[9] > '5' ||
        status_line[10] < '0' || status_line[10] > '9' ||
        status_line[11] < '0' || status_line[11] > '9' ||
        (status_line.size() > 12 && status_line[12] != ' '))
    {
        return {HttpError::MalformedResponse, 0, {}};
    }
    const int status_code = (status_line[9] - '0') * 100 +
                            (status_line[10] - '0') * 10 +
                            (status_line[11] - '0');

    std::optional<size_t> content_length;
    bool has_transfer_encoding = false;
    size_t cursor = status_end + 2;
    while (cursor < header_end)
    {
        size_t line_end = received.find("\r\n", cursor);
        if (line_end == std::string::npos || line_end > header_end)
        {
            line_end = header_end;
        }
        if (line_end == cursor)
        {
            return {HttpError::MalformedResponse, status_code, {}};
        }

        const std::string_view line(received.data() + cursor, line_end - cursor);
        const size_t separator = line.find(':');
        if (separator == std::string_view::npos || separator == 0)
        {
            return {HttpError::MalformedResponse, status_code, {}};
        }
        const std::string name = lowerAscii(line.substr(0, separator));
        const std::string_view value = line.substr(separator + 1);
        if (name == "content-length")
        {
            if (content_length)
            {
                return {HttpError::DuplicateContentLength, status_code, {}};
            }
            size_t parsed = 0;
            if (!parseContentLength(value, parsed))
            {
                return {HttpError::MalformedResponse, status_code, {}};
            }
            if (parsed > MAX_HTTP_BODY_BYTES)
            {
                return {HttpError::BodyTooLarge, status_code, {}};
            }
            content_length = parsed;
        }
        else if (name == "transfer-encoding")
        {
            has_transfer_encoding = true;
        }
        else if (name == "upgrade" || name == "content-encoding")
        {
            return {HttpError::MalformedResponse, status_code, {}};
        }
        cursor = line_end + 2;
    }

    if (has_transfer_encoding && content_length)
    {
        return {HttpError::MalformedResponse, status_code, {}};
    }
    if (has_transfer_encoding)
    {
        return {HttpError::UnsupportedTransferEncoding, status_code, {}};
    }
    if (!content_length)
    {
        return {HttpError::MissingContentLength, status_code, {}};
    }

    const size_t body_offset = header_end + 4;
    size_t body_bytes = received.size() - body_offset;
    if (body_bytes > *content_length)
    {
        return {HttpError::MalformedResponse, status_code, {}};
    }
    while (body_bytes < *content_length)
    {
        const size_t before = received.size();
        const HttpError result = receiveMore(HttpError::PrematureEof);
        if (result != HttpError::None)
        {
            return {result, status_code, {}};
        }
        body_bytes += received.size() - before;
        if (body_bytes > *content_length)
        {
            return {HttpError::MalformedResponse, status_code, {}};
        }
    }

    HttpResult result;
    result.status_code = status_code;
    result.body.assign(received.data() + body_offset, *content_length);
    if (status_code < 200 || status_code >= 300)
    {
        result.error = HttpError::HttpStatusError;
    }
    return result;
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
