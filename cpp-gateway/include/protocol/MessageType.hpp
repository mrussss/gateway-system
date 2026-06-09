#pragma once

#include <cstdint>

enum class MessageType : uint8_t
{
    PING = 1,
    ECHO = 2,
    LOG_PUSH = 3,
    STATS = 4,
    PONG = 5,
    ECHO_RESP = 6,
    ERROR_RESP = 7,
    LOG_ACK = 8,
    STATS_RESP = 9
};