#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "control/HttpTypes.hpp"

namespace control_detail
{
struct SocketReadResult
{
    HttpError error = HttpError::None;
    size_t size = 0;
    bool eof = false;
};

class SocketDeadline
{
public:
    using Deadline = std::chrono::steady_clock::time_point;

    ~SocketDeadline();
    SocketDeadline(const SocketDeadline &) = delete;
    SocketDeadline &operator=(const SocketDeadline &) = delete;
    SocketDeadline(SocketDeadline &&other) noexcept;
    SocketDeadline &operator=(SocketDeadline &&other) noexcept;

    static std::optional<SocketDeadline> connect(
        const std::string &host, int port, int timeout_ms,
        std::optional<Deadline> not_after, HttpError &error);

    HttpError sendAll(std::string_view data) const;
    SocketReadResult receive(char *buffer, size_t capacity) const;

private:
    SocketDeadline(int fd, Deadline deadline) noexcept;

    static std::optional<int> remainingMilliseconds(Deadline deadline);
    static HttpError waitForSocket(int fd, short events, Deadline deadline,
                                   HttpError failure);
    void closeSocket() noexcept;

    int fd_ = -1;
    Deadline deadline_{};
};
} // namespace control_detail
