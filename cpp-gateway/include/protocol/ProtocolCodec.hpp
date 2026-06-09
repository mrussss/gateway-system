#pragma once

#include <string>
#include <vector>

#include "protocol/Request.hpp"
#include "protocol/Response.hpp"

constexpr uint32_t MAX_PAYLOAD_SIZE = 4 * 1024 * 1024;

enum class DecodeStatus
{
    OK,
    NEED_MORE_DATA, // incomplete header or body
    INVALID_LENGTH, // invalid length guard (OOM prevention)
};

class ProtocolCodec
{
public:
    // 解码接口：
    static DecodeStatus decode(std::string &input_buffer, int fd,
                               std::vector<Request> &out_requests, uint64_t conn_id);

    // 编码接口：
    static std::string encode(const Response &response);
};