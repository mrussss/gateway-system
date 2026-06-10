#pragma once

#include <cstdint>
#include <string>
#include "protocol/MessageType.hpp"

struct Response
{
    int fd;
    uint64_t conn_id;
    uint8_t version;
    MessageType type;
    uint64_t request_id;
    int status_code = 0;
    std::string payload;
    bool mark_authenticated = false;
    std::string authenticated_client_id;
    bool close_connection = false;
    bool skip_write = false;
};
