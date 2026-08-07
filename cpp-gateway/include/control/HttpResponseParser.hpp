#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "control/HttpTypes.hpp"

namespace control_detail
{
class HttpResponseParser
{
public:
    HttpResponseParser();

    void consume(std::string_view chunk);
    void finishOnTransportError(HttpError error);
    void finishOnEof();
    bool done() const noexcept { return done_; }
    HttpResult takeResult();

private:
    bool parseHeaders(size_t header_end);
    void finishBodyIfComplete();
    void fail(HttpError error, int status_code = 0);

    std::string received_;
    std::optional<size_t> content_length_;
    size_t body_offset_ = 0;
    int status_code_ = 0;
    bool headers_parsed_ = false;
    bool done_ = false;
    HttpResult result_;
};
} // namespace control_detail
