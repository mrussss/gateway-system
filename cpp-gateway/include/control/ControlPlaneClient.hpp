#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "control/HttpTypes.hpp"
#include "control/RuntimeConfig.hpp"

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

class ControlPlaneClient
{
public:
    using Deadline = std::chrono::steady_clock::time_point;

    static constexpr size_t MAX_HTTP_HEADER_BYTES =
        MAX_CONTROL_PLANE_HTTP_HEADER_BYTES;
    static constexpr size_t MAX_HTTP_BODY_BYTES =
        MAX_CONTROL_PLANE_HTTP_BODY_BYTES;

    ControlPlaneClient(std::string host, int port, int timeout_ms,
                       std::string gateway_token = "");

    AuthResult checkAuth(const std::string &client_id, const std::string &token,
                         std::optional<Deadline> not_after = std::nullopt) const;
    bool fetchConfig(RuntimeConfig &config) const;

private:
    using Clock = std::chrono::steady_clock;

    HttpResult requestJson(std::string_view method, std::string_view path,
                           std::string_view body,
                           std::optional<Deadline> not_after = std::nullopt) const;
    HttpResult requestJsonOnce(std::string_view method, std::string_view path,
                               std::string_view body,
                               std::optional<Deadline> not_after) const;

    std::string hostHeader() const;
    static void validateHeaderValue(std::string_view name, std::string_view value);

    std::string host_;
    int port_;
    int timeout_ms_;
    std::string gateway_token_;
};
