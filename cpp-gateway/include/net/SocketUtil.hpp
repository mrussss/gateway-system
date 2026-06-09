#pragma once
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include "common/Logger.hpp"

inline bool setNonBlocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1)
    {
        LOG_ERROR("fcntl(F_GETFL) failed for fd=%d: %s", fd, strerror(errno));
        return false;
    }
    int new_flags = flags | O_NONBLOCK;
    int result = fcntl(fd, F_SETFL, new_flags);
    if (result == -1)
    {
        LOG_ERROR("fcntl(F_SETFL, O_NONBLOCK) failed for fd=%d: %s", fd, strerror(errno));
        return false;
    }
    return true;
}
