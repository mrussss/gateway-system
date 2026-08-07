#include "control/SocketDeadline.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdint>
#include <memory>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>

#include "common/Logger.hpp"

namespace control_detail
{
namespace
{
class UniqueFd
{
public:
    explicit UniqueFd(int fd) noexcept : fd_(fd)
    {
    }

    ~UniqueFd()
    {
        if (fd_ != -1)
        {
            close(fd_);
        }
    }

    UniqueFd(const UniqueFd &) = delete;
    UniqueFd &operator=(const UniqueFd &) = delete;

    explicit operator bool() const noexcept
    {
        return fd_ != -1;
    }

    int get() const noexcept
    {
        return fd_;
    }

    int release() noexcept
    {
        return std::exchange(fd_, -1);
    }

private:
    int fd_;
};

struct AddrInfoDeleter
{
    void operator()(addrinfo *value) const noexcept
    {
        if (value != nullptr)
        {
            freeaddrinfo(value);
        }
    }
};
} // namespace

SocketDeadline::SocketDeadline(int fd, Deadline deadline) noexcept
    : fd_(fd), deadline_(deadline)
{
}

SocketDeadline::~SocketDeadline()
{
    closeSocket();
}

SocketDeadline::SocketDeadline(SocketDeadline &&other) noexcept
    : fd_(std::exchange(other.fd_, -1)), deadline_(other.deadline_)
{
}

SocketDeadline &SocketDeadline::operator=(SocketDeadline &&other) noexcept
{
    if (this != &other)
    {
        closeSocket();
        fd_ = std::exchange(other.fd_, -1);
        deadline_ = other.deadline_;
    }
    return *this;
}

std::optional<SocketDeadline> SocketDeadline::connect(
    const std::string &host, int port, int timeout_ms,
    std::optional<Deadline> not_after, HttpError &error)
{
    error = HttpError::None;
    if (not_after && *not_after <= std::chrono::steady_clock::now())
    {
        error = HttpError::DeadlineExceeded;
        return std::nullopt;
    }

    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo *raw_addresses = nullptr;
    const std::string port_text = std::to_string(port);
    const int resolve_result = getaddrinfo(host.c_str(), port_text.c_str(), &hints,
                                           &raw_addresses);
    if (resolve_result != 0)
    {
        LOG_ERROR("control plane request failed: category=resolve host=%s port=%d error=%s",
                  host.c_str(), port, gai_strerror(resolve_result));
        error = HttpError::ResolveFailed;
        return std::nullopt;
    }
    std::unique_ptr<addrinfo, AddrInfoDeleter> addresses(raw_addresses);

    // Synchronous getaddrinfo is the documented boundary. Every resolved address
    // and all socket operations share the deadline created after resolution.
    Deadline deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(timeout_ms);
    if (not_after)
    {
        deadline = std::min(deadline, *not_after);
    }

    for (addrinfo *address = addresses.get(); address != nullptr;
         address = address->ai_next)
    {
        if (!remainingMilliseconds(deadline))
        {
            error = HttpError::DeadlineExceeded;
            return std::nullopt;
        }

        UniqueFd candidate(socket(address->ai_family,
                                  address->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                                  address->ai_protocol));
        if (!candidate)
        {
            continue;
        }

        const int result = ::connect(candidate.get(), address->ai_addr,
                                     address->ai_addrlen);
        if (result == -1 && errno != EINPROGRESS && errno != EINTR)
        {
            continue;
        }
        if (result == -1)
        {
            const HttpError waited = waitForSocket(candidate.get(), POLLOUT, deadline,
                                                   HttpError::ConnectFailed);
            if (waited == HttpError::DeadlineExceeded)
            {
                error = waited;
                return std::nullopt;
            }
            if (waited != HttpError::None)
            {
                continue;
            }

            int socket_error = 0;
            socklen_t socket_error_size = sizeof(socket_error);
            if (getsockopt(candidate.get(), SOL_SOCKET, SO_ERROR, &socket_error,
                           &socket_error_size) == -1 || socket_error != 0)
            {
                continue;
            }
        }
        return SocketDeadline(candidate.release(), deadline);
    }

    error = HttpError::ConnectFailed;
    return std::nullopt;
}

HttpError SocketDeadline::sendAll(std::string_view data) const
{
    size_t offset = 0;
    while (offset < data.size())
    {
        if (!remainingMilliseconds(deadline_))
        {
            return HttpError::DeadlineExceeded;
        }
        const ssize_t sent = send(fd_, data.data() + offset, data.size() - offset,
                                  MSG_NOSIGNAL);
        if (sent > 0)
        {
            offset += static_cast<size_t>(sent);
            continue;
        }
        if (sent == -1 && errno == EINTR)
        {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            const HttpError waited = waitForSocket(fd_, POLLOUT, deadline_,
                                                   HttpError::SendFailed);
            if (waited != HttpError::None)
            {
                return waited;
            }
            continue;
        }
        return HttpError::SendFailed;
    }
    return HttpError::None;
}

SocketReadResult SocketDeadline::receive(char *buffer, size_t capacity) const
{
    while (true)
    {
        if (!remainingMilliseconds(deadline_))
        {
            return {HttpError::DeadlineExceeded, 0, false};
        }
        const ssize_t count = recv(fd_, buffer, capacity, 0);
        if (count > 0)
        {
            return {HttpError::None, static_cast<size_t>(count), false};
        }
        if (count == 0)
        {
            return {HttpError::None, 0, true};
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            const HttpError waited = waitForSocket(fd_, POLLIN, deadline_,
                                                   HttpError::ReceiveFailed);
            if (waited != HttpError::None)
            {
                return {waited, 0, false};
            }
            continue;
        }
        return {HttpError::ReceiveFailed, 0, false};
    }
}

std::optional<int> SocketDeadline::remainingMilliseconds(Deadline deadline)
{
    const auto now = std::chrono::steady_clock::now();
    if (deadline <= now)
    {
        return std::nullopt;
    }
    const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(deadline - now);
    return static_cast<int>(std::min<int64_t>(remaining.count(), INT_MAX));
}

HttpError SocketDeadline::waitForSocket(int fd, short events, Deadline deadline,
                                        HttpError failure)
{
    while (true)
    {
        const auto timeout = remainingMilliseconds(deadline);
        if (!timeout)
        {
            return HttpError::DeadlineExceeded;
        }

        pollfd descriptor{};
        descriptor.fd = fd;
        descriptor.events = events;
        const int result = poll(&descriptor, 1, *timeout);
        if (result > 0)
        {
            if ((descriptor.revents & POLLNVAL) != 0)
            {
                return failure;
            }
            if ((descriptor.revents & (events | POLLERR | POLLHUP)) != 0)
            {
                return HttpError::None;
            }
            continue;
        }
        if (result == 0)
        {
            return HttpError::DeadlineExceeded;
        }
        if (errno != EINTR)
        {
            return failure;
        }
    }
}

void SocketDeadline::closeSocket() noexcept
{
    if (fd_ != -1)
    {
        close(fd_);
        fd_ = -1;
    }
}
} // namespace control_detail
