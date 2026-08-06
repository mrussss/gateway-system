#pragma once

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include "protocol/MessageType.hpp"
#include "protocol/Request.hpp"

struct AuthCancellation
{
    std::atomic<bool> cancelled{false};
};

struct AuthTask
{
    Request request;
    std::shared_ptr<AuthCancellation> cancellation;
};

inline std::optional<AuthTask> makeAuthTask(
    Request request, std::shared_ptr<AuthCancellation> cancellation)
{
    if (request.type != MessageType::AUTH || !cancellation)
    {
        return std::nullopt;
    }
    return AuthTask{std::move(request), std::move(cancellation)};
}
