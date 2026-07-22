#include "net/ReactorNotifier.hpp"

#include <cerrno>
#include <cstdint>
#include <sys/eventfd.h>
#include <unistd.h>

ReactorNotifier::~ReactorNotifier()
{
    close();
}

bool ReactorNotifier::open()
{
    if (fd_.load() != -1)
    {
        return true;
    }
    fd_.store(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    return fd_.load() != -1;
}

bool ReactorNotifier::notify() const noexcept
{
    const int descriptor = fd_.load();
    if (descriptor == -1)
    {
        return false;
    }

    const uint64_t value = 1;
    while (true)
    {
        ssize_t written = write(descriptor, &value, sizeof(value));
        if (written == static_cast<ssize_t>(sizeof(value)))
        {
            return true;
        }
        if (written == -1 && errno == EINTR)
        {
            continue;
        }
        // EAGAIN means the counter is already saturated, so the reactor is
        // guaranteed to observe a pending wakeup.
        return written == -1 && errno == EAGAIN;
    }
}

bool ReactorNotifier::consume() const noexcept
{
    const int descriptor = fd_.load();
    if (descriptor == -1)
    {
        return false;
    }

    uint64_t value = 0;
    while (true)
    {
        ssize_t n = read(descriptor, &value, sizeof(value));
        if (n == static_cast<ssize_t>(sizeof(value)))
        {
            continue;
        }
        if (n == -1 && errno == EINTR)
        {
            continue;
        }
        return n == -1 && errno == EAGAIN;
    }
}

void ReactorNotifier::close() noexcept
{
    const int descriptor = fd_.exchange(-1);
    if (descriptor != -1)
    {
        ::close(descriptor);
    }
}
