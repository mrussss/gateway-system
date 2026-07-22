#pragma once

#include <atomic>

class ReactorNotifier
{
public:
    ReactorNotifier() = default;
    ~ReactorNotifier();

    ReactorNotifier(const ReactorNotifier &) = delete;
    ReactorNotifier &operator=(const ReactorNotifier &) = delete;

    bool open();
    bool notify() const noexcept;
    bool consume() const noexcept;
    void close() noexcept;
    int fd() const noexcept { return fd_.load(); }

private:
    std::atomic<int> fd_{-1};
};
