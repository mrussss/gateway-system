#include "net/TcpServer.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "business/Dispatcher.hpp"
#include "business/Handlers.hpp"
#include "business/StatsManager.hpp"
#include "common/Logger.hpp"
#include "control/RuntimeConfig.hpp"
#include "protocol/ProtocolCodec.hpp"

namespace
{
constexpr uint32_t CLIENT_BASE_EVENTS = EPOLLET | EPOLLRDHUP;

class AtomicCounterGuard
{
public:
    explicit AtomicCounterGuard(std::atomic<size_t> &counter) : counter_(counter)
    {
        counter_.fetch_add(1, std::memory_order_relaxed);
    }
    ~AtomicCounterGuard()
    {
        counter_.fetch_sub(1, std::memory_order_relaxed);
    }

    AtomicCounterGuard(const AtomicCounterGuard &) = delete;
    AtomicCounterGuard &operator=(const AtomicCounterGuard &) = delete;

private:
    std::atomic<size_t> &counter_;
};

Response makeOverloadResponse(const Request &request, bool close_connection)
{
    Response response{};
    response.fd = request.fd;
    response.conn_id = request.conn_id;
    response.version = request.version;
    response.type = MessageType::ERROR_RESP;
    response.request_id = request.request_id;
    response.status_code = 503;
    response.payload = R"({"status":503,"message":"gateway overloaded"})";
    response.close_connection = close_connection;
    return response;
}

Response makeInternalErrorResponse(const Request &request)
{
    Response response{};
    response.fd = request.fd;
    response.conn_id = request.conn_id;
    response.version = request.version;
    response.type = MessageType::ERROR_RESP;
    response.request_id = request.request_id;
    response.status_code = 500;
    response.payload = R"({"status":500,"message":"internal server error"})";
    return response;
}

Response makeInternalAuthErrorResponse(const Request &request)
{
    Response response = makeInternalErrorResponse(request);
    response.type = MessageType::AUTH_RESP;
    response.payload = R"({"allowed":false,"code":"INTERNAL_ERROR"})";
    response.close_connection = true;
    return response;
}

#ifndef NDEBUG
void recordAdmittedWorkForTest(const Connection &connection)
{
    const char *path = std::getenv("WORKER_TEST_ADMITTED_FILE");
    const char *raw_target = std::getenv("WORKER_TEST_ADMITTED_TARGET");
    if (path == nullptr || *path == '\0' || raw_target == nullptr || *raw_target == '\0')
    {
        return;
    }

    char *end = nullptr;
    const long target = std::strtol(raw_target, &end, 10);
    if (end == raw_target || *end != '\0' || target <= 0 ||
        connection.in_flight_work < static_cast<size_t>(target))
    {
        return;
    }

    std::ofstream marker(path, std::ios::trunc);
    if (marker)
    {
        marker << connection.in_flight_work << '\n';
    }
}

long workerTestDelayMs(const char *name)
{
    const char *raw_delay = std::getenv(name);
    if (raw_delay == nullptr || *raw_delay == '\0')
    {
        return 0;
    }

    char *end = nullptr;
    const long delay_ms = std::strtol(raw_delay, &end, 10);
    if (end == raw_delay || *end != '\0' || delay_ms <= 0 || delay_ms > 60000)
    {
        return 0;
    }
    return delay_ms;
}

void applyWorkerTestDelay(const Request &request)
{
    const char *raw_request_id = std::getenv("WORKER_TEST_DELAY_REQUEST_ID");
    if (raw_request_id != nullptr && *raw_request_id != '\0')
    {
        char *end = nullptr;
        const unsigned long long request_id = std::strtoull(raw_request_id, &end, 10);
        if (end == raw_request_id || *end != '\0' ||
            request.request_id != static_cast<uint64_t>(request_id))
        {
            return;
        }
    }

    const long delay_ms = workerTestDelayMs("WORKER_TEST_DELAY_MS");
    if (delay_ms > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}
#endif
}

TcpServer *TcpServer::instance_ = nullptr;
volatile std::sig_atomic_t TcpServer::signal_stop_requested_ = 0;

TcpServer::TcpServer(int port)
    : TcpServer(port, "127.0.0.1", 8080)
{
}

TcpServer::TcpServer(int port, std::string control_plane_host, int control_plane_port)
    : TcpServer(port, std::move(control_plane_host), control_plane_port, 4096)
{
}

TcpServer::TcpServer(int port, std::string control_plane_host, int control_plane_port,
                     size_t request_queue_capacity,
                     size_t response_queue_capacity, int shutdown_timeout_ms,
                     unsigned int worker_count, std::string gateway_token,
                     int control_plane_timeout_ms, unsigned int auth_worker_count,
                     size_t auth_queue_capacity)
    : port_(port),
      shutdown_timeout_(std::max(shutdown_timeout_ms, 1)),
      configured_worker_count_(worker_count),
      configured_auth_worker_count_(auth_worker_count),
      request_queue_(std::max<size_t>(request_queue_capacity, 1)),
      auth_queue_(std::max<size_t>(auth_queue_capacity, 1)),
      response_queue_(std::max<size_t>(response_queue_capacity, 1)),
      control_plane_(std::move(control_plane_host), control_plane_port,
                     control_plane_timeout_ms,
                     std::move(gateway_token))
{
    if (configured_auth_worker_count_ == 0 || configured_auth_worker_count_ > 16)
    {
        throw std::invalid_argument("auth worker count must be in [1, 16]");
    }
    if (auth_queue_capacity == 0 || auth_queue_capacity > 65536)
    {
        throw std::invalid_argument("auth queue capacity must be in [1, 65536]");
    }
}

TcpServer::~TcpServer()
{
    stop();
}

void TcpServer::staticSignalHandler([[maybe_unused]] int signal_number)
{
    signal_stop_requested_ = 1;
    if (instance_ != nullptr)
    {
        instance_->notifier_.notify();
    }
}

void TcpServer::start()
{
    bool expected = false;
    if (!started_.compare_exchange_strong(expected, true))
    {
        throw std::logic_error("TcpServer::start may only be called once");
    }
    shutdown_finished_ = false;

    try
    {
        initServer();
    }
    catch (...)
    {
        closeListener();
        notifier_.close();
        if (epfd_ != -1)
        {
            close(epfd_);
            epfd_ = -1;
        }
        started_ = false;
        throw;
    }
    instance_ = this;
    signal_stop_requested_ = 0;
    state_ = ServerState::RUNNING;
    markReady();
    loop_thread_id_ = std::this_thread::get_id();

    if (std::signal(SIGINT, staticSignalHandler) == SIG_ERR ||
        std::signal(SIGTERM, staticSignalHandler) == SIG_ERR)
    {
        LOG_ERROR("%s", "failed to register SIGINT/SIGTERM handler");
    }
    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        LOG_ERROR("%s", "failed to ignore SIGPIPE");
    }

    unsigned int worker_count = configured_worker_count_;
    if (worker_count == 0)
    {
        worker_count = std::thread::hardware_concurrency();
        if (worker_count == 0)
        {
            worker_count = 4;
        }
        worker_count = std::min(worker_count, 4u);
    }
    response_producers_remaining_ = worker_count + configured_auth_worker_count_;
    LOG_INFO("gateway started: port=%d workers=%u auth_workers=%u request_capacity=%zu auth_capacity=%zu response_capacity=%zu",
             port_, worker_count, configured_auth_worker_count_, request_queue_.capacity(),
             auth_queue_.capacity(), response_queue_.capacity());

    for (unsigned int worker_id = 0; worker_id < worker_count; ++worker_id)
    {
        workers_.emplace_back([this, worker_id] { normalWorkerLoop(worker_id); });
    }
    for (unsigned int worker_id = 0; worker_id < configured_auth_worker_count_; ++worker_id)
    {
        auth_workers_.emplace_back([this, worker_id] { authWorkerLoop(worker_id); });
    }

    startConfigPuller();
    loop();
    finishShutdown();
}

void TcpServer::normalWorkerLoop(unsigned int worker_id)
{
    business::Dispatcher dispatcher;
    Request request{};
    while (request_queue_.pop(request))
    {
        try
        {
#ifndef NDEBUG
            applyWorkerTestDelay(request);
#endif
            enqueueWorkerResponse(dispatcher.dispatch(request), ResponseProducer::NormalWorker);
        }
        catch (const std::exception &error)
        {
            business::StatsManager::getInstance().incrementErrors();
            LOG_ERROR("worker=%u dispatch failed: %s", worker_id, error.what());
            enqueueWorkerResponse(makeInternalErrorResponse(request),
                                  ResponseProducer::NormalWorker);
        }
        catch (...)
        {
            business::StatsManager::getInstance().incrementErrors();
            LOG_ERROR("worker=%u dispatch failed with unknown exception", worker_id);
            enqueueWorkerResponse(makeInternalErrorResponse(request),
                                  ResponseProducer::NormalWorker);
        }
    }
    LOG_DEBUG("worker=%u drained request queue and exited", worker_id);
    onResponseProducerExited();
}

void TcpServer::authWorkerLoop(unsigned int worker_id)
{
    AuthTask task{};
    while (auth_queue_.pop(task))
    {
        if (task.cancellation &&
            task.cancellation->cancelled.load(std::memory_order_relaxed))
        {
            business::StatsManager::getInstance().incrementAuthTaskCancelledBeforeStart();
            continue;
        }

        const auto started_at = std::chrono::steady_clock::now();
        AtomicCounterGuard in_flight(auth_in_flight_);
        try
        {
            business::StatsManager::getInstance().incrementRequests();
            std::optional<ControlPlaneClient::Deadline> not_after;
            if (state_.load() != ServerState::RUNNING)
            {
                not_after = shutdownDeadline();
            }
            business::AuthHandlingResult handled =
                business::handleAuth(task.request, control_plane_, not_after);
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started_at);
            business::StatsManager::getInstance().recordAuthResult(
                handled.outcome, static_cast<uint64_t>(duration.count()));
            enqueueWorkerResponse(std::move(handled.response), ResponseProducer::AuthWorker);
        }
        catch (const std::exception &error)
        {
            business::StatsManager::getInstance().incrementErrors();
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started_at);
            business::StatsManager::getInstance().recordAuthResult(
                AuthOutcome::Unavailable, static_cast<uint64_t>(duration.count()));
            LOG_ERROR("auth_worker=%u failed: %s", worker_id, error.what());
            enqueueWorkerResponse(makeInternalAuthErrorResponse(task.request),
                                  ResponseProducer::AuthWorker);
        }
        catch (...)
        {
            business::StatsManager::getInstance().incrementErrors();
            const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - started_at);
            business::StatsManager::getInstance().recordAuthResult(
                AuthOutcome::Unavailable, static_cast<uint64_t>(duration.count()));
            LOG_ERROR("auth_worker=%u failed with unknown exception", worker_id);
            enqueueWorkerResponse(makeInternalAuthErrorResponse(task.request),
                                  ResponseProducer::AuthWorker);
        }
    }
    LOG_DEBUG("auth_worker=%u drained auth queue and exited", worker_id);
    onResponseProducerExited();
}

void TcpServer::onResponseProducerExited()
{
    if (response_producers_remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1)
    {
        response_queue_.stop();
        notifier_.notify();
    }
}

void TcpServer::stop()
{
    if (!started_.load())
    {
        state_ = ServerState::STOPPED;
        shutdown_finished_ = true;
        return;
    }
    if (shutdown_finished_.load())
    {
        return;
    }

    stop_requested_ = true;
    notifier_.notify();
    if (std::this_thread::get_id() == loop_thread_id_)
    {
        return;
    }

    std::unique_lock<std::mutex> lock(stopped_mutex_);
    stopped_cv_.wait(lock, [this]
                     { return shutdown_finished_.load(); });
}

void TcpServer::initServer()
{
    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(static_cast<uint16_t>(port_));

    listen_fd_ = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (listen_fd_ == -1)
    {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    int enabled = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &enabled, sizeof(enabled)) == -1)
    {
        throw std::runtime_error(std::string("setsockopt(SO_REUSEADDR) failed: ") +
                                 std::strerror(errno));
    }
    if (bind(listen_fd_, reinterpret_cast<sockaddr *>(&server_address),
             sizeof(server_address)) == -1)
    {
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }
    if (listen(listen_fd_, 128) == -1)
    {
        throw std::runtime_error(std::string("listen failed: ") + std::strerror(errno));
    }

    epfd_ = epoll_create1(EPOLL_CLOEXEC);
    if (epfd_ == -1)
    {
        throw std::runtime_error(std::string("epoll_create1 failed: ") + std::strerror(errno));
    }
    if (!notifier_.open())
    {
        throw std::runtime_error(std::string("eventfd failed: ") + std::strerror(errno));
    }

    epoll_event listener_event{};
    listener_event.events = EPOLLIN | EPOLLET;
    listener_event.data.fd = listen_fd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &listener_event) == -1)
    {
        throw std::runtime_error(std::string("epoll add listener failed: ") +
                                 std::strerror(errno));
    }

    epoll_event notifier_event{};
    notifier_event.events = EPOLLIN;
    notifier_event.data.fd = notifier_.fd();
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, notifier_.fd(), &notifier_event) == -1)
    {
        throw std::runtime_error(std::string("epoll add eventfd failed: ") +
                                 std::strerror(errno));
    }
}

void TcpServer::loop()
{
    assertReactorThread();
    epoll_event events[1024];
    while (state_.load() != ServerState::STOPPED)
    {
        if ((stop_requested_.load() || signal_stop_requested_ != 0) &&
            state_.load() == ServerState::RUNNING)
        {
            beginDraining();
        }

        int event_count = epoll_wait(epfd_, events, 1024, epollTimeoutMs());
        if (event_count == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            LOG_ERROR("epoll_wait failed: %s", std::strerror(errno));
            beginDraining();
            continue;
        }

        for (int index = 0; index < event_count; ++index)
        {
            const int fd = events[index].data.fd;
            const uint32_t flags = events[index].events;
            if (fd == notifier_.fd())
            {
                notifier_.consume();
                continue;
            }
            if (fd == listen_fd_)
            {
                if (state_.load() == ServerState::RUNNING)
                {
                    handleAccept();
                }
                continue;
            }

            if ((flags & EPOLLERR) != 0)
            {
                LOG_DEBUG("closing fd=%d after epoll error flags=0x%x", fd, flags);
                closeConnection(fd);
                continue;
            }
            if ((flags & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0 &&
                state_.load() == ServerState::RUNNING)
            {
                handleRead(fd);
            }
            if (connections_.find(fd) == connections_.end())
            {
                continue;
            }
            if ((flags & (EPOLLRDHUP | EPOLLHUP)) != 0)
            {
                markReadEof(fd);
            }
            if ((flags & EPOLLOUT) != 0)
            {
                handleWrite(fd);
            }
        }

        drainResponseQueue();
        drainRejectedResponses();
        if (state_.load() == ServerState::DRAINING)
        {
            if (drainComplete())
            {
                LOG_INFO("%s", "graceful shutdown drain completed");
                state_ = ServerState::STOPPED;
            }
            else if (std::chrono::steady_clock::now() >= shutdownDeadline())
            {
                const size_t discarded_requests = request_queue_.abort();
                const size_t discarded_auth_tasks = auth_queue_.abort();
                const size_t discarded_responses = response_queue_.abort();
                LOG_ERROR("graceful shutdown deadline reached: discarded_requests=%zu discarded_auth_tasks=%zu discarded_responses=%zu connections=%zu auth_in_flight=%zu",
                          discarded_requests, discarded_auth_tasks, discarded_responses,
                          connections_.size(), auth_in_flight_.load());
                state_ = ServerState::STOPPED;
            }
        }
    }
}

void TcpServer::beginDraining()
{
    assertReactorThread();
    std::unique_lock<std::mutex> deadline_lock(shutdown_deadline_mutex_);
    ServerState expected = ServerState::RUNNING;
    if (!state_.compare_exchange_strong(expected, ServerState::DRAINING))
    {
        return;
    }

    shutdown_deadline_ = std::chrono::steady_clock::now() + shutdown_timeout_;
    deadline_lock.unlock();
    markNotReady();
    closeListener();
    request_queue_.stop();
    auth_queue_.stop();
    background_wait_cv_.notify_all();
    LOG_INFO("graceful shutdown started: deadline_ms=%lld queued_requests=%zu queued_auth=%zu",
             static_cast<long long>(shutdown_timeout_.count()), request_queue_.size(),
             auth_queue_.size());

    std::vector<std::pair<int, uint32_t>> connection_events;
    connection_events.reserve(connections_.size());
    for (const auto &[fd, connection] : connections_)
    {
        connection_events.emplace_back(fd, connectionEvents(connection));
    }
    for (const auto &[fd, events] : connection_events)
    {
        modifyConnectionEvents(fd, events);
    }
}

void TcpServer::finishShutdown()
{
    assertReactorThread();
    markNotReady();
    request_queue_.stop();
    auth_queue_.stop();
    response_queue_.stop();
    background_wait_cv_.notify_all();

    if (config_puller_.joinable())
    {
        config_puller_.join();
    }
    for (auto &worker : workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }
    for (auto &worker : auth_workers_)
    {
        if (worker.joinable())
        {
            worker.join();
        }
    }

    std::vector<int> fds;
    fds.reserve(connections_.size());
    for (const auto &[fd, connection] : connections_)
    {
        (void)connection;
        fds.push_back(fd);
    }
    for (int fd : fds)
    {
        closeConnection(fd);
    }
    closeListener();
    notifier_.close();
    if (epfd_ != -1)
    {
        close(epfd_);
        epfd_ = -1;
    }

    instance_ = nullptr;
    state_ = ServerState::STOPPED;
    shutdown_finished_ = true;
    {
        std::lock_guard<std::mutex> lock(stopped_mutex_);
    }
    stopped_cv_.notify_all();
    LOG_INFO("%s", "gateway shutdown complete");
}

void TcpServer::markReady()
{
    std::ofstream ready(readiness_file_, std::ios::trunc);
    if (!ready) { LOG_ERROR("failed to create readiness file: %s", readiness_file_.c_str()); }
}

void TcpServer::markNotReady()
{
    if (unlink(readiness_file_.c_str()) == -1 && errno != ENOENT)
    {
        LOG_ERROR("failed to remove readiness file: %s", std::strerror(errno));
    }
}

bool TcpServer::drainComplete()
{
    assertReactorThread();
    if (response_producers_remaining_.load() != 0 || !request_queue_.stopped() ||
        request_queue_.size() != 0 || !auth_queue_.stopped() || auth_queue_.size() != 0 ||
        !response_queue_.stopped() || response_queue_.size() != 0)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(rejected_responses_mutex_);
        if (!rejected_response_connections_.empty())
        {
            return false;
        }
    }

    std::vector<int> completed;
    for (const auto &[fd, connection] : connections_)
    {
        if (canCloseAfterDrain(connection))
        {
            completed.push_back(fd);
        }
    }
    for (int fd : completed)
    {
        closeConnection(fd);
    }

    return connections_.empty();
}

int TcpServer::epollTimeoutMs() const
{
    if (state_.load() != ServerState::DRAINING)
    {
        return -1;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        shutdownDeadline() - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
    {
        return 0;
    }
    return static_cast<int>(std::min<int64_t>(remaining.count(),
                                              std::numeric_limits<int>::max()));
}

ControlPlaneClient::Deadline TcpServer::shutdownDeadline() const
{
    std::lock_guard<std::mutex> lock(shutdown_deadline_mutex_);
    return shutdown_deadline_;
}

void TcpServer::closeListener()
{
    if (listen_fd_ == -1)
    {
        return;
    }
    if (epfd_ != -1)
    {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, listen_fd_, nullptr);
    }
    close(listen_fd_);
    listen_fd_ = -1;
}

void TcpServer::handleAccept()
{
    assertReactorThread();
    while (state_.load() == ServerState::RUNNING)
    {
        sockaddr_in client_address{};
        socklen_t address_length = sizeof(client_address);
        int fd = accept4(listen_fd_, reinterpret_cast<sockaddr *>(&client_address),
                         &address_length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            LOG_ERROR("accept4 failed: %s", std::strerror(errno));
            return;
        }

        const uint64_t connection_id = next_conn_id_.fetch_add(1);
        epoll_event event{};
        Connection connection(fd, connection_id);
        event.events = connectionEvents(connection);
        event.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event) == -1)
        {
            LOG_ERROR("epoll add client failed: fd=%d error=%s", fd, std::strerror(errno));
            close(fd);
            continue;
        }

        connections_.emplace(fd, std::move(connection));
        business::StatsManager::getInstance().incrementConnections();
        LOG_DEBUG("accepted fd=%d conn_id=%llu", fd,
                  static_cast<unsigned long long>(connection_id));
    }
}

void TcpServer::handleRead(int fd)
{
    assertReactorThread();
    while (state_.load() == ServerState::RUNNING)
    {
        const auto connection = connections_.find(fd);
        if (connection == connections_.end() || connection->second.closing ||
            connection->second.read_eof)
        {
            return;
        }
        char buffer[4096];
        ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes_read > 0)
        {
            auto connection = connections_.find(fd);
            if (connection == connections_.end())
            {
                return;
            }
            connection->second.input_buffer.append(buffer, static_cast<size_t>(bytes_read));
            business::StatsManager::getInstance().incrementReadBytes(
                static_cast<size_t>(bytes_read));
            LOG_DEBUG("read fd=%d conn_id=%llu bytes=%zd", fd,
                      static_cast<unsigned long long>(connection->second.conn_id), bytes_read);
            continue;
        }
        if (bytes_read == 0)
        {
            decodeAndEnqueue(fd, true);
            markReadEof(fd);
            return;
        }
        if (errno == EINTR)
        {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            decodeAndEnqueue(fd);
            return;
        }
        LOG_ERROR("recv failed: fd=%d error=%s", fd, std::strerror(errno));
        closeConnection(fd);
        return;
    }
}

void TcpServer::markReadEof(int fd)
{
    assertReactorThread();
    auto connection = connections_.find(fd);
    if (connection == connections_.end())
    {
        return;
    }

    Connection &current = connection->second;
    if (current.read_eof)
    {
        maybeCloseAfterDrain(fd);
        return;
    }
    current.read_eof = true;
    LOG_DEBUG("peer read EOF fd=%d conn_id=%llu in_flight=%zu", fd,
              static_cast<unsigned long long>(current.conn_id), current.in_flight_work);

    if (canCloseAfterDrain(current))
    {
        closeConnection(fd);
        return;
    }
    modifyConnectionEvents(fd, connectionEvents(current));
}

void TcpServer::maybeCloseAfterDrain(int fd)
{
    assertReactorThread();
    auto connection = connections_.find(fd);
    if (connection != connections_.end() &&
        canCloseAfterDrain(connection->second))
    {
        closeConnection(fd);
    }
}

void TcpServer::handleWrite(int fd)
{
    assertReactorThread();
    while (true)
    {
        bool output_empty = false;
        ssize_t sent = 0;
        auto connection = connections_.find(fd);
        if (connection == connections_.end())
        {
            return;
        }
        Connection &current = connection->second;
        output_empty = current.write_offset >= current.output_buffer.size();
        if (!output_empty)
        {
            sent = send(fd, current.output_buffer.data() + current.write_offset,
                        current.output_buffer.size() - current.write_offset, MSG_NOSIGNAL);
            if (sent > 0)
            {
                current.write_offset += static_cast<size_t>(sent);
                business::StatsManager::getInstance().incrementWriteBytes(
                    static_cast<size_t>(sent));
                if (current.write_offset == current.output_buffer.size())
                {
                    current.output_buffer.clear();
                    current.write_offset = 0;
                    output_empty = true;
                }
            }
        }

        if (output_empty)
        {
            if (canCloseAfterDrain(current))
            {
                closeConnection(fd);
                return;
            }
            modifyConnectionEvents(fd, connectionEvents(current));
            return;
        }
        if (sent > 0)
        {
            continue;
        }
        if (sent == -1 && errno == EINTR)
        {
            continue;
        }
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }
        LOG_ERROR("send failed: fd=%d error=%s", fd, std::strerror(errno));
        closeConnection(fd);
        return;
    }
}

bool TcpServer::enqueueWorkerResponse(Response response, ResponseProducer producer)
{
    const int fd = response.fd;
    const uint64_t connection_id = response.conn_id;
    const uint64_t request_id = response.request_id;
    const MessageType type = response.type;
    const size_t payload_size = response.payload.size();
    const PushResult result = response_queue_.push(std::move(response));
    if (result == PushResult::OK)
    {
        LOG_DEBUG("response queued fd=%d conn_id=%llu type=%d request_id=%llu bytes=%zu",
                  fd, static_cast<unsigned long long>(connection_id), static_cast<int>(type),
                  static_cast<unsigned long long>(request_id), payload_size);
#ifndef NDEBUG
        const bool suppress_notify =
            producer == ResponseProducer::NormalWorker &&
            std::getenv("WORKER_TEST_SUPPRESS_NORMAL_RESPONSE_NOTIFY") != nullptr;
        if (producer == ResponseProducer::NormalWorker)
        {
            const long delay_ms = workerTestDelayMs("WORKER_TEST_NORMAL_NOTIFY_DELAY_MS");
            if (delay_ms > 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
            }
        }
        if (!suppress_notify)
        {
            notifier_.notify();
        }
#else
        notifier_.notify();
#endif
        return true;
    }

    business::StatsManager::getInstance().incrementErrors();
    if (producer == ResponseProducer::AuthWorker)
    {
        business::StatsManager::getInstance().incrementResponseQueueRejectedAuth();
    }
    else
    {
        business::StatsManager::getInstance().incrementResponseQueueRejectedNormal();
    }
    LOG_ERROR("response queue rejected item: fd=%d conn_id=%llu result=%s", fd,
              static_cast<unsigned long long>(connection_id),
              result == PushResult::FULL ? "full" : "stopped");
    {
        std::lock_guard<std::mutex> lock(rejected_responses_mutex_);
        rejected_response_connections_.insert(ConnectionKey{fd, connection_id});
    }
#ifndef NDEBUG
    if (producer != ResponseProducer::NormalWorker ||
        std::getenv("WORKER_TEST_SUPPRESS_NORMAL_RESPONSE_NOTIFY") == nullptr)
    {
        notifier_.notify();
    }
#else
    notifier_.notify();
#endif
    return false;
}

void TcpServer::drainResponseQueue()
{
    assertReactorThread();
    Response response{};
    while (response_queue_.tryPop(response))
    {
        applyResponse(std::move(response), ResponseOrigin::Worker);
    }
}

void TcpServer::drainRejectedResponses()
{
    assertReactorThread();
    std::unordered_set<ConnectionKey, ConnectionKeyHash> rejected;
    {
        std::lock_guard<std::mutex> lock(rejected_responses_mutex_);
        rejected.swap(rejected_response_connections_);
    }
    for (const ConnectionKey &key : rejected)
    {
        auto connection = connections_.find(key.fd);
        if (connection != connections_.end() && connection->second.conn_id == key.conn_id)
        {
            closeConnection(key.fd);
        }
    }
}

void TcpServer::applyResponse(Response response, ResponseOrigin origin)
{
    assertReactorThread();
    bool close_now = false;
    bool enable_write = false;
    uint32_t events_after_response = 0;
    RuntimeConfig config = getRuntimeConfigSnapshot();
    auto connection = connections_.find(response.fd);
    if (connection == connections_.end() || connection->second.conn_id != response.conn_id)
    {
        business::StatsManager::getInstance().incrementStaleResponseDropped();
        LOG_DEBUG("discarding stale response fd=%d conn_id=%llu", response.fd,
                  static_cast<unsigned long long>(response.conn_id));
        return;
    }

    Connection &current = connection->second;
    if (origin == ResponseOrigin::Worker)
    {
        if (current.in_flight_work == 0)
        {
            LOG_ERROR("worker completion without admitted work: fd=%d conn_id=%llu",
                      current.fd, static_cast<unsigned long long>(current.conn_id));
#ifndef NDEBUG
            assert(false && "in_flight_work accounting violation");
#endif
        }
        else
        {
            --current.in_flight_work;
        }
    }
    if (response.type == MessageType::AUTH_RESP || response.close_connection)
    {
        current.auth_pending = false;
        current.auth_cancellation.reset();
    }
    if (response.client_id_to_authenticate)
    {
        const size_t authenticated = countAuthenticatedConnectionsForClient(
            *response.client_id_to_authenticate, current.fd);
        if (authenticated >= static_cast<size_t>(config.max_connections_per_client))
        {
            business::StatsManager::getInstance().incrementErrors();
            response.client_id_to_authenticate.reset();
            response.close_connection = true;
            response.type = MessageType::AUTH_RESP;
            response.payload = R"({"allowed":false,"reason":"max connections exceeded"})";
            business::StatsManager::getInstance().incrementAuthFailure();
        }
        else
        {
            current.authenticated = true;
            current.client_id = *response.client_id_to_authenticate;
            business::StatsManager::getInstance().incrementAuthSuccess();
        }
    }
    else if (response.type == MessageType::AUTH_RESP)
    {
        business::StatsManager::getInstance().incrementAuthFailure();
    }

    if (response.close_connection && response.skip_write)
    {
        close_now = true;
    }
    else
    {
        std::string encoded = ProtocolCodec::encode(response);
        const size_t pending_bytes = current.output_buffer.size() - current.write_offset;
        if (pending_bytes + encoded.size() > config.slow_client_output_limit)
        {
            business::StatsManager::getInstance().incrementErrors();
            business::StatsManager::getInstance().incrementSlowClientClosed();
            LOG_ERROR("output buffer limit exceeded: fd=%d conn_id=%llu pending=%zu new=%zu",
                      current.fd, static_cast<unsigned long long>(current.conn_id),
                      pending_bytes, encoded.size());
            close_now = true;
        }
        else
        {
            if (current.write_offset > 0)
            {
                current.output_buffer.erase(0, current.write_offset);
                current.write_offset = 0;
            }
            current.output_buffer.append(encoded);
            current.closing = current.closing || response.close_connection;
            events_after_response = connectionEvents(current);
            enable_write = true;
        }
    }

    if (close_now)
    {
        closeConnection(response.fd);
    }
    else if (enable_write)
    {
        modifyConnectionEvents(response.fd, events_after_response);
    }
    else
    {
        maybeCloseAfterDrain(response.fd);
    }
}

bool TcpServer::decodeAndEnqueue(int fd, bool read_eof)
{
    assertReactorThread();
    std::vector<Request> decoded_requests;
    std::vector<Request> requests_to_enqueue;
    std::vector<AuthTask> auth_tasks_to_enqueue;
    std::vector<Response> local_responses;
    bool close_now = false;
    RuntimeConfig config = getRuntimeConfigSnapshot();

    auto connection = connections_.find(fd);
    if (connection == connections_.end())
    {
        return false;
    }
    Connection &current = connection->second;
    const bool eof = read_eof || current.read_eof;
    const DecodeStatus status = ProtocolCodec::decode(
        current.input_buffer, current.fd, decoded_requests, current.conn_id);
    if (status == DecodeStatus::INVALID_LENGTH)
    {
        business::StatsManager::getInstance().incrementErrors();
        close_now = true;
    }
    else if (eof && status == DecodeStatus::NEED_MORE_DATA)
    {
        LOG_ERROR("truncated frame at peer EOF: fd=%d conn_id=%llu trailing_bytes=%zu", fd,
                  static_cast<unsigned long long>(current.conn_id),
                  current.input_buffer.size());
        current.input_buffer.clear();
    }

    for (auto &request : decoded_requests)
    {
        if (request.payload.size() + 10 > static_cast<size_t>(config.max_payload_size))
        {
            business::StatsManager::getInstance().incrementErrors();
            close_now = true;
            break;
        }
        if (!current.authenticated)
        {
            if (request.type != MessageType::AUTH || current.auth_pending)
            {
                business::StatsManager::getInstance().incrementErrors();
                close_now = true;
                break;
            }
            current.auth_pending = true;
            auto cancellation = std::make_shared<AuthCancellation>();
            auto task = makeAuthTask(std::move(request), cancellation);
            if (!task)
            {
                business::StatsManager::getInstance().incrementErrors();
                close_now = true;
                break;
            }
            current.auth_cancellation = std::move(cancellation);
            auth_tasks_to_enqueue.push_back(std::move(*task));
            continue;
        }
        if (request.type == MessageType::AUTH)
        {
            Response response{};
            response.fd = current.fd;
            response.conn_id = current.conn_id;
            response.version = request.version;
            response.type = MessageType::ERROR_RESP;
            response.request_id = request.request_id;
            response.status_code = 400;
            response.payload = R"({"status":400,"message":"already authenticated"})";
            local_responses.push_back(std::move(response));
            continue;
        }
        if (!allowRequestForClient(current.client_id, config))
        {
            business::StatsManager::getInstance().incrementErrors();
            Response response{};
            response.fd = current.fd;
            response.conn_id = current.conn_id;
            response.version = request.version;
            response.type = MessageType::ERROR_RESP;
            response.request_id = request.request_id;
            response.status_code = 429;
            response.payload = R"({"status":429,"message":"rate limited"})";
            local_responses.push_back(std::move(response));
            continue;
        }
        requests_to_enqueue.push_back(std::move(request));
    }

    if (close_now)
    {
        closeConnection(fd);
        return false;
    }

    for (auto &task : auth_tasks_to_enqueue)
    {
        const int task_fd = task.request.fd;
        const uint64_t task_conn_id = task.request.conn_id;
        const PushResult result = auth_queue_.push(std::move(task));
        if (result == PushResult::OK)
        {
            auto connection = connections_.find(task_fd);
            if (connection != connections_.end() && connection->second.conn_id == task_conn_id)
            {
                ++connection->second.in_flight_work;
            }
            continue;
        }
        if (result != PushResult::OK)
        {
            business::StatsManager::getInstance().incrementErrors();
            business::StatsManager::getInstance().incrementAuthQueueRejected();
            business::StatsManager::getInstance().recordAuthResult(AuthOutcome::Unavailable, 0);
            LOG_ERROR("auth queue rejected item: fd=%d conn_id=%llu result=%s",
                      task.request.fd,
                      static_cast<unsigned long long>(task.request.conn_id),
                      result == PushResult::FULL ? "full" : "stopped");
            Response overload = makeOverloadResponse(task.request, true);
            overload.type = MessageType::AUTH_RESP;
            overload.payload = R"({"allowed":false,"code":"AUTH_OVERLOADED"})";
            local_responses.push_back(std::move(overload));
        }
    }
    for (auto &request : requests_to_enqueue)
    {
        const int request_fd = request.fd;
        const uint64_t request_conn_id = request.conn_id;
        const PushResult result = request_queue_.push(std::move(request));
        if (result == PushResult::OK)
        {
            auto connection = connections_.find(request_fd);
            if (connection != connections_.end() && connection->second.conn_id == request_conn_id)
            {
                ++connection->second.in_flight_work;
#ifndef NDEBUG
                recordAdmittedWorkForTest(connection->second);
#endif
            }
            continue;
        }
        if (result != PushResult::OK)
        {
            business::StatsManager::getInstance().incrementErrors();
            business::StatsManager::getInstance().incrementRequestQueueRejected();
            LOG_ERROR("request queue rejected item: fd=%d conn_id=%llu result=%s",
                      request.fd, static_cast<unsigned long long>(request.conn_id),
                      result == PushResult::FULL ? "full" : "stopped");
            local_responses.push_back(makeOverloadResponse(request, false));
        }
    }
    for (auto &response : local_responses)
    {
        applyResponse(std::move(response), ResponseOrigin::Local);
    }
    return true;
}

bool TcpServer::modifyConnectionEvents(int fd, uint32_t events)
{
    assertReactorThread();
    epoll_event event{};
    event.events = events;
    event.data.fd = fd;
    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) == -1)
    {
        if (errno != ENOENT && errno != EBADF)
        {
            LOG_ERROR("epoll modify failed: fd=%d error=%s", fd, std::strerror(errno));
        }
        closeConnection(fd);
        return false;
    }
    return true;
}

uint32_t TcpServer::connectionEvents(const Connection &connection) const
{
    uint32_t events = CLIENT_BASE_EVENTS;
    if (state_.load() == ServerState::RUNNING && !connection.closing &&
        !connection.read_eof)
    {
        events |= EPOLLIN;
    }
    if (connection.write_offset < connection.output_buffer.size())
    {
        events |= EPOLLOUT;
    }
    return events;
}

bool TcpServer::canCloseAfterDrain(const Connection &connection) const
{
    const bool output_empty = connection.write_offset >= connection.output_buffer.size();
    const bool drain_requested = connection.closing || connection.read_eof ||
                                 state_.load() == ServerState::DRAINING;
    return drain_requested && connection.in_flight_work == 0 && output_empty;
}

void TcpServer::assertReactorThread() const
{
#ifndef NDEBUG
    assert(loop_thread_id_ == std::this_thread::get_id());
#endif
}

void TcpServer::startConfigPuller()
{
    config_puller_ = std::thread([this]
    {
        configPullerLoop();
    });
}

void TcpServer::configPullerLoop()
{
    while (state_.load() == ServerState::RUNNING)
    {
        RuntimeConfig fetched;
        if (control_plane_.fetchConfig(fetched))
        {
            std::lock_guard<std::mutex> lock(runtime_config_mutex_);
            const int64_t previous_version = runtime_config_.version;
            if (applyRuntimeConfigIfNewer(runtime_config_, fetched))
            {
                setGatewayLogLevel(runtime_config_.log_level);
                LOG_INFO("runtime config updated: version=%lld->%lld",
                         static_cast<long long>(previous_version),
                         static_cast<long long>(runtime_config_.version));
            }
        }
        else
        {
            LOG_ERROR("%s", "runtime config fetch failed; retaining current snapshot");
        }
        std::unique_lock<std::mutex> lock(background_wait_mutex_);
        background_wait_cv_.wait_for(lock, std::chrono::seconds(5), [this]
        {
            return state_.load() != ServerState::RUNNING;
        });
    }
}

RuntimeConfig TcpServer::getRuntimeConfigSnapshot()
{
    std::lock_guard<std::mutex> lock(runtime_config_mutex_);
    return runtime_config_;
}

size_t TcpServer::countAuthenticatedConnectionsForClient(const std::string &client_id,
                                                          int exclude_fd) const
{
    size_t count = 0;
    for (const auto &[fd, connection] : connections_)
    {
        if (fd != exclude_fd && connection.authenticated && connection.client_id == client_id)
        {
            ++count;
        }
    }
    return count;
}

bool TcpServer::allowRequestForClient(const std::string &client_id,
                                      const RuntimeConfig &config)
{
    const auto now = std::chrono::steady_clock::now();
    RateLimitWindow &window = rate_limit_windows_[client_id];
    if (window.started_at == std::chrono::steady_clock::time_point{} ||
        now - window.started_at >= std::chrono::seconds(1))
    {
        window.started_at = now;
        window.count = 0;
    }
    if (window.count >= config.max_requests_per_client_per_second)
    {
        return false;
    }
    ++window.count;
    return true;
}

void TcpServer::closeConnection(int fd)
{
    assertReactorThread();
    bool existed = false;
    std::string client_id;
    bool authenticated = false;
    auto connection = connections_.find(fd);
    if (connection == connections_.end())
    {
        return;
    }
    client_id = connection->second.client_id;
    authenticated = connection->second.authenticated;
    if (connection->second.auth_cancellation)
    {
        connection->second.auth_cancellation->cancelled.store(
            true, std::memory_order_relaxed);
    }
    connections_.erase(connection);
    if (authenticated && countAuthenticatedConnectionsForClient(client_id, -1) == 0)
    {
        rate_limit_windows_.erase(client_id);
    }
    existed = true;
    if (epfd_ != -1)
    {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }
    close(fd);
    if (existed)
    {
        business::StatsManager::getInstance().decrementConnections();
    }
}
