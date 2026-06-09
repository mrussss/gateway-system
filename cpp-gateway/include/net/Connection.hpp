#pragma once

#include <cstdint>
#include <string>
#include <utility>

struct Connection
{
    int fd;
    uint64_t conn_id;
    std::string input_buffer;
    std::string output_buffer;
    std::string client_id;
    std::string remote_addr;
    std::string connected_at;
    bool authenticated = false;
    bool closing = false;

    Connection(int fd_, uint64_t conn_id_, std::string remote_addr_, std::string connected_at_)
        : fd(fd_),
          conn_id(conn_id_),
          input_buffer(""),
          client_id("client_" + std::to_string(conn_id_)),
          remote_addr(std::move(remote_addr_)),
          connected_at(std::move(connected_at_))
    {
    }
};
