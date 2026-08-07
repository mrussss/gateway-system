#pragma once

#include <cstddef>
#include <string>

inline constexpr size_t MAX_CONTROL_PLANE_HTTP_HEADER_BYTES = 16 * 1024;
inline constexpr size_t MAX_CONTROL_PLANE_HTTP_BODY_BYTES = 1024 * 1024;

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
