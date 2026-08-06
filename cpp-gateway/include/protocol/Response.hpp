#pragma once

#include <cstdint>
#include <optional>
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
    std::optional<std::string> client_id_to_authenticate;
    bool close_connection = false;
    bool skip_write = false;
};
