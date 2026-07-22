#include "TestHarness.hpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <sys/epoll.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "net/ReactorNotifier.hpp"

int main()
{
    return runTests({
        {"worker notification wakes epoll", []
         {
             ReactorNotifier notifier;
             CHECK(notifier.open());
             int epfd = epoll_create1(EPOLL_CLOEXEC);
             CHECK(epfd != -1);
             epoll_event registration{};
             registration.events = EPOLLIN;
             registration.data.fd = notifier.fd();
             CHECK_EQ(epoll_ctl(epfd, EPOLL_CTL_ADD, notifier.fd(), &registration), 0);

             std::thread worker([&]
             {
                 std::this_thread::sleep_for(std::chrono::milliseconds(10));
                 CHECK(notifier.notify());
             });
             epoll_event event{};
             CHECK_EQ(epoll_wait(epfd, &event, 1, 1000), 1);
             CHECK_EQ(event.data.fd, notifier.fd());
             CHECK(notifier.consume());
             worker.join();
             close(epfd);
         }},
        {"burst notifications coalesce without losing wakeup", []
         {
             ReactorNotifier notifier;
             CHECK(notifier.open());
             constexpr int worker_count = 8;
             constexpr int notifications_per_worker = 1000;
             std::vector<std::thread> workers;
             for (int i = 0; i < worker_count; ++i)
             {
                 workers.emplace_back([&]
                 {
                     for (int n = 0; n < notifications_per_worker; ++n)
                     {
                         CHECK(notifier.notify());
                     }
                 });
             }
             for (auto &worker : workers)
             {
                 worker.join();
             }
             CHECK(notifier.consume());
             errno = 0;
             uint64_t value = 0;
             CHECK_EQ(read(notifier.fd(), &value, sizeof(value)), ssize_t{-1});
             CHECK_EQ(errno, EAGAIN);
         }},
    });
}
