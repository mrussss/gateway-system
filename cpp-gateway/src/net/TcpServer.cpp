#include "net/TcpServer.hpp"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <fstream>
#include <limits>
#include <netinet/in.h>
#include <sstream>
#include <stdexcept>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

#include "business/Dispatcher.hpp"
#include "business/StatsManager.hpp"
#include "common/Logger.hpp"
#include "control/RuntimeConfig.hpp"
#include "protocol/ProtocolCodec.hpp"

namespace
{
constexpr uint32_t CLIENT_BASE_EVENTS = EPOLLET | EPOLLRDHUP;

std::string generateBootId()
{
    std::ifstream input("/proc/sys/kernel/random/uuid");
    std::string value;
    if (input >> value) { return value; }
    return std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + std::to_string(getpid());
}

std::string formatUtcTime(std::chrono::system_clock::time_point time_point)
{
    std::time_t time = std::chrono::system_clock::to_time_t(time_point);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string formatRemoteAddr(const sockaddr_in &address)
{
    char ip[INET_ADDRSTRLEN] = {0};
    if (inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip)) == nullptr)
    {
        return "unknown";
    }
    std::ostringstream output;
    output << ip << ':' << ntohs(address.sin_port);
    return output.str();
}

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
}

TcpServer *TcpServer::instance_ = nullptr;
volatile std::sig_atomic_t TcpServer::signal_stop_requested_ = 0;

TcpServer::TcpServer(int port)
    : TcpServer(port, "127.0.0.1", 8080, "gateway-001")
{
}

TcpServer::TcpServer(int port, std::string control_plane_host, int control_plane_port)
    : TcpServer(port, std::move(control_plane_host), control_plane_port, "gateway-001")
{
}

TcpServer::TcpServer(int port, std::string control_plane_host, int control_plane_port,
                     std::string gateway_id, size_t request_queue_capacity,
                     size_t response_queue_capacity, int shutdown_timeout_ms,
                     unsigned int worker_count, std::string gateway_token)
    : port_(port),
      shutdown_timeout_(std::max(shutdown_timeout_ms, 1)),
      configured_worker_count_(worker_count),
      request_queue_(std::max<size_t>(request_queue_capacity, 1)),
      response_queue_(std::max<size_t>(response_queue_capacity, 1)),
      control_plane_(std::move(control_plane_host), control_plane_port, 1000,
                     std::move(gateway_token)),
      gateway_id_(std::move(gateway_id)), gateway_boot_id_(generateBootId()),
      process_start_time_(std::time(nullptr))
{
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
    workers_remaining_ = worker_count;
    LOG_INFO("gateway started: port=%d workers=%u request_capacity=%zu response_capacity=%zu",
             port_, worker_count, request_queue_.capacity(), response_queue_.capacity());

    for (unsigned int worker_id = 0; worker_id < worker_count; ++worker_id)
    {
        workers_.emplace_back([this, worker_id]
        {
            business::Dispatcher dispatcher(control_plane_);
            Request request{};
            while (request_queue_.pop(request))
            {
                try
                {
                    Response response = dispatcher.dispatch(request);
                    enqueueWorkerResponse(std::move(response));
                }
                catch (const std::exception &error)
                {
                    business::StatsManager::getInstance().incrementErrors();
                    LOG_ERROR("worker=%u dispatch failed: %s", worker_id, error.what());
                    enqueueWorkerResponse(makeInternalErrorResponse(request));
                }
                catch (...)
                {
                    business::StatsManager::getInstance().incrementErrors();
                    LOG_ERROR("worker=%u dispatch failed with unknown exception", worker_id);
                    enqueueWorkerResponse(makeInternalErrorResponse(request));
                }
            }

            LOG_DEBUG("worker=%u drained request queue and exited", worker_id);
            if (workers_remaining_.fetch_sub(1) == 1)
            {
                response_queue_.stop();
                notifier_.notify();
            }
        });
    }

    startMetricsReporter();
    startConfigPuller();
    loop();
    finishShutdown();
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

            if ((flags & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0)
            {
                LOG_DEBUG("closing fd=%d after epoll flags=0x%x", fd, flags);
                closeConnection(fd);
                continue;
            }
            if ((flags & EPOLLIN) != 0 && state_.load() == ServerState::RUNNING)
            {
                handleRead(fd);
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
            else if (std::chrono::steady_clock::now() >= shutdown_deadline_)
            {
                const size_t discarded_requests = request_queue_.abort();
                const size_t discarded_responses = response_queue_.abort();
                LOG_ERROR("graceful shutdown deadline reached: discarded_requests=%zu discarded_responses=%zu connections=%zu",
                          discarded_requests, discarded_responses, connections_.size());
                state_ = ServerState::STOPPED;
            }
        }
    }
}

void TcpServer::beginDraining()
{
    ServerState expected = ServerState::RUNNING;
    if (!state_.compare_exchange_strong(expected, ServerState::DRAINING))
    {
        return;
    }

    shutdown_deadline_ = std::chrono::steady_clock::now() + shutdown_timeout_;
    markNotReady();
    closeListener();
    request_queue_.stop();
    LOG_INFO("graceful shutdown started: deadline_ms=%lld queued_requests=%zu",
             static_cast<long long>(shutdown_timeout_.count()), request_queue_.size());

    std::vector<std::pair<int, bool>> connection_states;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        connection_states.reserve(connections_.size());
        for (const auto &[fd, connection] : connections_)
        {
            const bool wants_write = connection.write_offset < connection.output_buffer.size();
            connection_states.emplace_back(fd, wants_write);
        }
    }
    for (const auto &[fd, wants_write] : connection_states)
    {
        const uint32_t write_event = wants_write ? static_cast<uint32_t>(EPOLLOUT) : 0U;
        modifyConnectionEvents(fd, CLIENT_BASE_EVENTS | write_event);
    }
}

void TcpServer::finishShutdown()
{
    markNotReady();
    request_queue_.stop();
    response_queue_.stop();

    if (metrics_reporter_.joinable())
    {
        metrics_reporter_.join();
    }
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

    std::vector<int> fds;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        fds.reserve(connections_.size());
        for (const auto &[fd, connection] : connections_)
        {
            (void)connection;
            fds.push_back(fd);
        }
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
    if (workers_remaining_.load() != 0 || !response_queue_.stopped() ||
        response_queue_.size() != 0)
    {
        return false;
    }

    std::vector<int> completed;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto &[fd, connection] : connections_)
        {
            if (connection.write_offset >= connection.output_buffer.size())
            {
                completed.push_back(fd);
            }
        }
    }
    for (int fd : completed)
    {
        closeConnection(fd);
    }

    std::lock_guard<std::mutex> lock(connections_mutex_);
    return connections_.empty();
}

int TcpServer::epollTimeoutMs() const
{
    if (state_.load() != ServerState::DRAINING)
    {
        return -1;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        shutdown_deadline_ - std::chrono::steady_clock::now());
    if (remaining.count() <= 0)
    {
        return 0;
    }
    return static_cast<int>(std::min<int64_t>(remaining.count(),
                                              std::numeric_limits<int>::max()));
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

        epoll_event event{};
        event.events = connectionEvents(false);
        event.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event) == -1)
        {
            LOG_ERROR("epoll add client failed: fd=%d error=%s", fd, std::strerror(errno));
            close(fd);
            continue;
        }

        const uint64_t connection_id = next_conn_id_.fetch_add(1);
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.emplace(fd, Connection(fd, connection_id,
                                                 formatRemoteAddr(client_address),
                                                 formatUtcTime(std::chrono::system_clock::now())));
        }
        business::StatsManager::getInstance().incrementConnections();
        LOG_DEBUG("accepted fd=%d conn_id=%llu", fd,
                  static_cast<unsigned long long>(connection_id));
    }
}

void TcpServer::handleRead(int fd)
{
    while (state_.load() == ServerState::RUNNING)
    {
        char buffer[4096];
        ssize_t bytes_read = recv(fd, buffer, sizeof(buffer), 0);
        if (bytes_read > 0)
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
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
            closeConnection(fd);
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

void TcpServer::handleWrite(int fd)
{
    while (true)
    {
        bool close_after_write = false;
        bool output_empty = false;
        ssize_t sent = 0;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto connection = connections_.find(fd);
            if (connection == connections_.end())
            {
                return;
            }
            Connection &current = connection->second;
            output_empty = current.write_offset >= current.output_buffer.size();
            close_after_write = current.closing;
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
        }

        if (output_empty)
        {
            if (close_after_write || state_.load() == ServerState::DRAINING)
            {
                closeConnection(fd);
                return;
            }
            modifyConnectionEvents(fd, connectionEvents(false));
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

bool TcpServer::enqueueWorkerResponse(Response response)
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
        notifier_.notify();
        return true;
    }

    business::StatsManager::getInstance().incrementErrors();
    business::StatsManager::getInstance().incrementResponseQueueRejected();
    LOG_ERROR("response queue rejected item: fd=%d conn_id=%llu result=%s", fd,
              static_cast<unsigned long long>(connection_id),
              result == PushResult::FULL ? "full" : "stopped");
    {
        std::lock_guard<std::mutex> lock(rejected_responses_mutex_);
        rejected_response_connections_[fd] = connection_id;
    }
    notifier_.notify();
    return false;
}

void TcpServer::drainResponseQueue()
{
    Response response{};
    while (response_queue_.tryPop(response))
    {
        applyResponse(std::move(response));
    }
}

void TcpServer::drainRejectedResponses()
{
    std::unordered_map<int, uint64_t> rejected;
    {
        std::lock_guard<std::mutex> lock(rejected_responses_mutex_);
        rejected.swap(rejected_response_connections_);
    }
    for (const auto &[fd, connection_id] : rejected)
    {
        bool matches = false;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto connection = connections_.find(fd);
            matches = connection != connections_.end() &&
                      connection->second.conn_id == connection_id;
        }
        if (matches)
        {
            closeConnection(fd);
        }
    }
}

void TcpServer::applyResponse(Response response)
{
    bool close_now = false;
    bool enable_write = false;
    RuntimeConfig config = getRuntimeConfigSnapshot();
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto connection = connections_.find(response.fd);
        if (connection == connections_.end() ||
            connection->second.conn_id != response.conn_id)
        {
            business::StatsManager::getInstance().incrementStaleResponseDropped();
            LOG_DEBUG("discarding stale response fd=%d conn_id=%llu", response.fd,
                      static_cast<unsigned long long>(response.conn_id));
            return;
        }

        Connection &current = connection->second;
        if (response.type == MessageType::AUTH_RESP || response.close_connection)
        {
            current.auth_pending = false;
        }
        if (response.mark_authenticated)
        {
            const size_t authenticated = countAuthenticatedConnectionsForClientLocked(
                response.authenticated_client_id, current.fd);
            if (authenticated >= static_cast<size_t>(config.max_connections_per_client))
            {
                business::StatsManager::getInstance().incrementErrors();
                response.mark_authenticated = false;
                response.close_connection = true;
                response.type = MessageType::AUTH_RESP;
                response.payload =
                    R"({"allowed":false,"reason":"max connections exceeded"})";
                business::StatsManager::getInstance().incrementAuthFailure();
            }
            else
            {
                current.authenticated = true;
                current.client_id = response.authenticated_client_id;
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
                enable_write = true;
            }
        }
    }

    if (close_now)
    {
        closeConnection(response.fd);
    }
    else if (enable_write)
    {
        modifyConnectionEvents(response.fd, connectionEvents(true));
    }
}

bool TcpServer::decodeAndEnqueue(int fd)
{
    std::vector<Request> decoded_requests;
    std::vector<Request> requests_to_enqueue;
    std::vector<Response> local_responses;
    bool close_now = false;
    RuntimeConfig config = getRuntimeConfigSnapshot();

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto connection = connections_.find(fd);
        if (connection == connections_.end())
        {
            return false;
        }
        Connection &current = connection->second;
        const DecodeStatus status = ProtocolCodec::decode(
            current.input_buffer, current.fd, decoded_requests, current.conn_id);
        if (status == DecodeStatus::INVALID_LENGTH)
        {
            business::StatsManager::getInstance().incrementErrors();
            close_now = true;
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
                requests_to_enqueue.push_back(std::move(request));
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
            if (!allowRequestForClientLocked(current.client_id, config))
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
    }

    if (close_now)
    {
        closeConnection(fd);
        return false;
    }

    for (auto &request : requests_to_enqueue)
    {
        const bool auth_request = request.type == MessageType::AUTH;
        const PushResult result = request_queue_.push(request);
        if (result != PushResult::OK)
        {
            business::StatsManager::getInstance().incrementErrors();
            business::StatsManager::getInstance().incrementRequestQueueRejected();
            LOG_ERROR("request queue rejected item: fd=%d conn_id=%llu result=%s",
                      request.fd, static_cast<unsigned long long>(request.conn_id),
                      result == PushResult::FULL ? "full" : "stopped");
            Response overload = makeOverloadResponse(request, auth_request);
            if (auth_request)
            {
                overload.type = MessageType::AUTH_RESP;
            }
            local_responses.push_back(std::move(overload));
        }
    }
    for (auto &response : local_responses)
    {
        applyResponse(std::move(response));
    }
    return true;
}

bool TcpServer::modifyConnectionEvents(int fd, uint32_t events)
{
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

uint32_t TcpServer::connectionEvents(bool wants_write) const
{
    uint32_t events = CLIENT_BASE_EVENTS;
    if (state_.load() == ServerState::RUNNING)
    {
        events |= EPOLLIN;
    }
    if (wants_write)
    {
        events |= EPOLLOUT;
    }
    return events;
}

void TcpServer::startMetricsReporter()
{
    metrics_reporter_ = std::thread([this]
    {
        metricsReporterLoop();
    });
}

void TcpServer::startConfigPuller()
{
    RuntimeConfig fetched;
    if (control_plane_.fetchConfig(fetched))
    {
        std::lock_guard<std::mutex> lock(runtime_config_mutex_);
        runtime_config_ = fetched;
        setGatewayLogLevel(runtime_config_.log_level);
        LOG_INFO("runtime config initialized: version=%lld",
                 static_cast<long long>(runtime_config_.version));
    }
    else
    {
        LOG_ERROR("runtime config startup fetch failed; retaining version=%lld",
                  static_cast<long long>(runtime_config_.version));
    }
    config_puller_ = std::thread([this]
    {
        configPullerLoop();
    });
}

void TcpServer::metricsReporterLoop()
{
    while (state_.load() == ServerState::RUNNING)
    {
        auto &stats = business::StatsManager::getInstance();
        const auto snapshot = stats.snapshot();
        const RuntimeConfig config = getRuntimeConfigSnapshot();
        GatewayMetrics metrics{
            gateway_id_, gateway_boot_id_, process_start_time_, snapshot.active_connections,
            snapshot.total_requests, snapshot.bytes_in, snapshot.bytes_out, snapshot.errors,
            request_queue_.capacity(), request_queue_.size(), request_queue_.peakSize(),
            snapshot.request_queue_rejected, response_queue_.capacity(), response_queue_.size(),
            response_queue_.peakSize(), snapshot.response_queue_rejected,
            snapshot.slow_client_closed, snapshot.stale_response_dropped,
            snapshot.auth_success, snapshot.auth_failure, config.version, "RUNNING",
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())};
        control_plane_.reportMetrics(metrics);
        control_plane_.reportClients(metrics.gateway_id, buildClientSnapshot());
        for (int tick = 0; tick < 50 && state_.load() == ServerState::RUNNING; ++tick)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
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
        for (int tick = 0; tick < 50 && state_.load() == ServerState::RUNNING; ++tick)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

RuntimeConfig TcpServer::getRuntimeConfigSnapshot()
{
    std::lock_guard<std::mutex> lock(runtime_config_mutex_);
    return runtime_config_;
}

size_t TcpServer::countAuthenticatedConnectionsForClientLocked(const std::string &client_id,
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

bool TcpServer::allowRequestForClientLocked(const std::string &client_id,
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

std::vector<ClientReport> TcpServer::buildClientSnapshot()
{
    std::vector<ClientReport> clients;
    std::lock_guard<std::mutex> lock(connections_mutex_);
    clients.reserve(connections_.size());
    for (const auto &[fd, connection] : connections_)
    {
        (void)fd;
        if (connection.authenticated)
        {
            clients.push_back({connection.client_id, connection.remote_addr,
                               connection.connected_at});
        }
    }
    return clients;
}

void TcpServer::closeConnection(int fd)
{
    bool existed = false;
    std::string client_id;
    bool authenticated = false;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto connection = connections_.find(fd);
        if (connection == connections_.end())
        {
            return;
        }
        client_id = connection->second.client_id;
        authenticated = connection->second.authenticated;
        connections_.erase(connection);
        if (authenticated && countAuthenticatedConnectionsForClientLocked(client_id, -1) == 0)
        {
            rate_limit_windows_.erase(client_id);
        }
        existed = true;
    }
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
