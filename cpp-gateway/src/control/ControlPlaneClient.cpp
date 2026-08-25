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
        logHttpFailure("config", response);
        return false;
    }
    config = parsed;
    return true;
}

HttpResult ControlPlaneClient::requestJson(std::string_view method, std::string_view path,
                                           std::string_view body,
                                           std::optional<Deadline> not_after) const
{
    return requestJsonOnce(method, path, body, not_after);
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
