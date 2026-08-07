#include "control/HttpResponseParser.hpp"

#include <algorithm>
#include <cctype>
#include <utility>

namespace control_detail
{
namespace
{
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
        if (parsed > (MAX_CONTROL_PLANE_HTTP_BODY_BYTES - digit) / 10)
        {
            value = MAX_CONTROL_PLANE_HTTP_BODY_BYTES + 1;
            return true;
        }
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}
} // namespace

HttpResponseParser::HttpResponseParser()
{
    received_.reserve(MAX_CONTROL_PLANE_HTTP_HEADER_BYTES);
}

void HttpResponseParser::consume(std::string_view chunk)
{
    if (done_)
    {
        return;
    }
    received_.append(chunk);

    if (!headers_parsed_)
    {
        const size_t header_end = received_.find("\r\n\r\n");
        if (header_end == std::string::npos)
        {
            if (received_.size() > MAX_CONTROL_PLANE_HTTP_HEADER_BYTES)
            {
                fail(HttpError::HeaderTooLarge);
            }
            return;
        }
        if (header_end + 4 > MAX_CONTROL_PLANE_HTTP_HEADER_BYTES)
        {
            fail(HttpError::HeaderTooLarge);
            return;
        }
        if (!parseHeaders(header_end))
        {
            return;
        }
        body_offset_ = header_end + 4;
        headers_parsed_ = true;
    }

    finishBodyIfComplete();
}

void HttpResponseParser::finishOnTransportError(HttpError error)
{
    if (!done_)
    {
        fail(error, status_code_);
    }
}

void HttpResponseParser::finishOnEof()
{
    if (!done_)
    {
        fail(headers_parsed_ ? HttpError::PrematureEof
                             : HttpError::MalformedResponse,
             status_code_);
    }
}

HttpResult HttpResponseParser::takeResult()
{
    return std::move(result_);
}

bool HttpResponseParser::parseHeaders(size_t header_end)
{
    const size_t status_end = received_.find("\r\n");
    if (status_end == std::string::npos || status_end >= header_end)
    {
        fail(HttpError::MalformedResponse);
        return false;
    }

    const std::string_view status_line(received_.data(), status_end);
    if (status_line.size() < 12 ||
        (status_line.substr(0, 8) != "HTTP/1.0" &&
         status_line.substr(0, 8) != "HTTP/1.1") ||
        status_line[8] != ' ' ||
        status_line[9] < '1' || status_line[9] > '5' ||
        status_line[10] < '0' || status_line[10] > '9' ||
        status_line[11] < '0' || status_line[11] > '9' ||
        (status_line.size() > 12 && status_line[12] != ' '))
    {
        fail(HttpError::MalformedResponse);
        return false;
    }
    status_code_ = (status_line[9] - '0') * 100 +
                   (status_line[10] - '0') * 10 +
                   (status_line[11] - '0');

    bool has_transfer_encoding = false;
    size_t cursor = status_end + 2;
    while (cursor < header_end)
    {
        size_t line_end = received_.find("\r\n", cursor);
        if (line_end == std::string::npos || line_end > header_end)
        {
            line_end = header_end;
        }
        if (line_end == cursor)
        {
            fail(HttpError::MalformedResponse, status_code_);
            return false;
        }

        const std::string_view line(received_.data() + cursor, line_end - cursor);
        const size_t separator = line.find(':');
        if (separator == std::string_view::npos || separator == 0)
        {
            fail(HttpError::MalformedResponse, status_code_);
            return false;
        }
        const std::string name = lowerAscii(line.substr(0, separator));
        const std::string_view value = line.substr(separator + 1);
        if (name == "content-length")
        {
            if (content_length_)
            {
                fail(HttpError::DuplicateContentLength, status_code_);
                return false;
            }
            size_t parsed = 0;
            if (!parseContentLength(value, parsed))
            {
                fail(HttpError::MalformedResponse, status_code_);
                return false;
            }
            if (parsed > MAX_CONTROL_PLANE_HTTP_BODY_BYTES)
            {
                fail(HttpError::BodyTooLarge, status_code_);
                return false;
            }
            content_length_ = parsed;
        }
        else if (name == "transfer-encoding")
        {
            has_transfer_encoding = true;
        }
        else if (name == "upgrade" || name == "content-encoding")
        {
            fail(HttpError::MalformedResponse, status_code_);
            return false;
        }
        cursor = line_end + 2;
    }

    if (has_transfer_encoding && content_length_)
    {
        fail(HttpError::MalformedResponse, status_code_);
        return false;
    }
    if (has_transfer_encoding)
    {
        fail(HttpError::UnsupportedTransferEncoding, status_code_);
        return false;
    }
    if (!content_length_)
    {
        fail(HttpError::MissingContentLength, status_code_);
        return false;
    }
    return true;
}

void HttpResponseParser::finishBodyIfComplete()
{
    const size_t body_bytes = received_.size() - body_offset_;
    if (body_bytes > *content_length_)
    {
        fail(HttpError::MalformedResponse, status_code_);
        return;
    }
    if (body_bytes < *content_length_)
    {
        return;
    }

    result_.status_code = status_code_;
    result_.body.assign(received_.data() + body_offset_, *content_length_);
    if (status_code_ < 200 || status_code_ >= 300)
    {
        result_.error = HttpError::HttpStatusError;
    }
    done_ = true;
}

void HttpResponseParser::fail(HttpError error, int status_code)
{
    result_.error = error;
    result_.status_code = status_code;
    result_.body.clear();
    done_ = true;
}
} // namespace control_detail
