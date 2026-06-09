#pragma once

struct Connection
{
    int fd;
    uint64_t conn_id;
    std::string input_buffer;
    std::string output_buffer;
    bool closing = false;

    Connection(int fd_, uint64_t conn_id_) : fd(fd_), conn_id(conn_id_), input_buffer("") {}
};
