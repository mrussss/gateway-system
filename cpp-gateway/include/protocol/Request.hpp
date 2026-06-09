#pragma once

#include <cstdint>
#include <string>
#include "protocol/MessageType.hpp"

struct Request
{

    int fd;
    uint64_t conn_id;
    uint8_t version;
    MessageType type;
    uint64_t request_id;
    std::string payload;
};
