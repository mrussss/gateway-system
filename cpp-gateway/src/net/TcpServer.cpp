#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cerrno>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <arpa/inet.h>
#include "concurrent/BlockQueue.hpp"
#include "net/TcpServer.hpp"
#include "net/SocketUtil.hpp"
#include "protocol/Request.hpp"
#include "protocol/ProtocolCodec.hpp"
#include "business/Dispatcher.hpp"
#include "business/StatsManager.hpp"
#include "common/Logger.hpp"
#include "nlohmann/json.hpp"

namespace
{
    constexpr size_t MAX_OUT_BUFFER_SIZE = 2 * 1024 * 1024;

    std::string formatUtcTime(std::chrono::system_clock::time_point tp)
    {
        std::time_t time = std::chrono::system_clock::to_time_t(tp);
        std::tm tm{};
        gmtime_r(&time, &tm);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return oss.str();
    }

    std::string formatRemoteAddr(const sockaddr_in &addr)
    {
        char ip[INET_ADDRSTRLEN] = {0};
        if (inet_ntop(AF_INET, &addr.sin_addr, ip, sizeof(ip)) == nullptr)
        {
            return "unknown";
        }

        std::ostringstream oss;
        oss << ip << ":" << ntohs(addr.sin_port);
        return oss.str();
    }
}

// =========================================================
// 1. Construction & Destruction
// =========================================================
TcpServer::TcpServer(int port) : TcpServer(port, "127.0.0.1", 8080, "gateway-001") {}

TcpServer::TcpServer(int port, std::string control_plane_host, int control_plane_port)
    : TcpServer(port, std::move(control_plane_host), control_plane_port, "gateway-001")
{
}

TcpServer::TcpServer(int port, std::string control_plane_host, int control_plane_port, std::string gateway_id)
    : port_(port),
      listen_fd_(-1),
      epfd_(-1),
      running_(false),
      control_plane_(std::move(control_plane_host), control_plane_port, 1000),
      gateway_id_(std::move(gateway_id))
{
}

TcpServer::~TcpServer()
{
    stop();
}

// =========================================================
// 2. Server Start
// =========================================================

TcpServer *TcpServer::instance_ = nullptr;
void TcpServer::static_sigint_handler([[maybe_unused]] int sig)
{
    if (instance_)
    {
        instance_->running_ = false;
    }
}
void TcpServer::start()
{
    instance_ = this;
    if (std::signal(SIGINT, static_sigint_handler) == SIG_ERR)
    {
        std::cerr << "signal registration failed" << std::endl;
    }
    std::cout << "C++ server: press Ctrl+C to exit" << std::endl;

    if (std::signal(SIGPIPE, SIG_IGN) == SIG_ERR)
    {
        std::cerr << "failed to ignore SIGPIPE" << std::endl;
    }
    else
    {
        LOG_INFO("%s", "SIGPIPE ignored");
    }

    initServer();
    running_ = true;

    unsigned int hw_cores = std::thread::hardware_concurrency();
    if (hw_cores == 0)
    {
        hw_cores = 4;
    }

    unsigned int worker_count = std::min(hw_cores, 4u);
    LOG_INFO("System detected %u cores, starting %u worker threads...", hw_cores, worker_count);
    for (unsigned i = 0; i < worker_count; ++i)
    {
        workers_.emplace_back([this, worker_id = i]()
                              {
                                  try
                                  {
                                      business::Dispatcher Dispatch(control_plane_);
                                      while (true)
                                      {
                                          Request req;
                                          bool ok = request_queue_.pop(req);
                                          if (!ok)
                                          {
                                              LOG_INFO("Worker %u received shutdown signal, exiting gracefully.", worker_id);
                                              break;
                                          }
                                          try
                                          {
                                              Response resp = Dispatch.dispatch(req);
                                              response_queue_.push(resp);
                                              LOG_INFO("Worker %u generated Response! fd=%d, type=%d, id=%llu, payload=%s",
                                                       worker_id,
                                                       resp.fd,
                                                       static_cast<int>(resp.type),
                                                       (unsigned long long)resp.request_id,
                                                       resp.payload.c_str());
                                          }
                                          catch (const std::exception &e)
                                          {
                                              std::cerr << "failed to push response: " << e.what() << std::endl;
                                          }
                                          catch (...)
                                          {
                                              std::cerr << "unknown error pushing response!" << std::endl;
                                          }
                                      }
                                  }
                                  catch (const std::exception &e)
                                  {
                                      std::cerr << "worker thread failed: " << e.what() << std::endl;
                                  }
                                  catch (...)
                                  {
                                      std::cerr << "worker thread unknown error!" << std::endl;
                                  } });
    }
    startMetricsReporter();
    startConfigPuller();
    loop();
}

void TcpServer::stop()
{
    if (is_stopped_)
    {
        return;
    }
    is_stopped_ = true;
    running_ = false;

    if (listen_fd_ != -1)
    {
        close(listen_fd_);
        listen_fd_ = -1;
    }
    std::vector<int> fds_to_close;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        for (const auto &[fd, conn] : connections_)
        {
            (void)conn;
            fds_to_close.push_back(fd);
        }
    }
    for (int fd : fds_to_close)
    {
        closeConnection(fd);
    }
    request_queue_.stop();
    response_queue_.stop();
    LOG_INFO("task queue stopped, waiting for all workers to exit");

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
    if (epfd_ != -1)
    {
        close(epfd_);
        epfd_ = -1;
    }

    LOG_INFO("all worker threads exited, system shutdown complete.");

    return;
}

// =========================================================
// 3. Network Initialization
// =========================================================
void TcpServer::initServer()
{
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port_);

    listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd_ == -1)
    {
        perror("socket failed");
        exit(1);
    }

    int opt = 1;
    if (setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        LOG_INFO("[FATAL] setsockopt(SO_REUSEADDR) failed! Port might be locked.");
        exit(EXIT_FAILURE);
    }
    LOG_INFO("[INFO] SO_REUSEADDR enabled.");

    int bind_return = bind(listen_fd_, (struct sockaddr *)&server_addr, sizeof(server_addr));
    if (bind_return == -1)
    {
        perror("bind failed");
        exit(1);
    }
    int listen_retuen = listen(listen_fd_, 10);
    if (listen_retuen == -1)
    {
        perror("listen failed");
        exit(1);
    }
    setNonBlocking(listen_fd_);

    epfd_ = epoll_create1(0);
    if (epfd_ == -1)
    {
        perror("epoll_creael failed");
        exit(EXIT_FAILURE);
    }
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = EPOLLIN | EPOLLET;
    event.data.fd = listen_fd_;
    if (epoll_ctl(epfd_, EPOLL_CTL_ADD, listen_fd_, &event) == -1)
    {
        perror("epoll_ctl:listen_fd_ add failed");
        close(epfd_);
        exit(EXIT_FAILURE);
    }
    LOG_INFO("Success: listen_fd_ successfully mounted to epoll instance with EPOLLET.");
}

void TcpServer::loop()
{

    epoll_event events[1024];

    while (running_)
    {
        int nfds = epoll_wait(epfd_, events, 1024, 100);
        if (nfds == -1)
        {
            if (errno == EINTR)
            {
                continue;
            }
            perror("epoll_wait error");
            break;
        }
        for (int i = 0; i < nfds; i++)
        {
            int fd = events[i].data.fd;
            if (fd == listen_fd_)
            {
                handleAccept();
            }
            else
            {
                if (events[i].events & EPOLLIN)
                {
                    handleRead(fd);
                }
                if (events[i].events & EPOLLOUT)
                {
                    handleWrite(fd);
                }
            }
        }
        drainResponseQueue();
    }
}

// =========================================================
// 4. I/O Event Handlers
// =========================================================
void TcpServer::handleAccept()
{
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    int fd;
    while (true)
    {
        fd = accept(listen_fd_, (struct sockaddr *)&client_addr, &addr_len);
        if (fd == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            else
            {
                perror("accept error");
                break;
            }
        }
        setNonBlocking(fd);

        struct epoll_event event1;
        memset(&event1, 0, sizeof(event1));
        event1.events = EPOLLIN | EPOLLET;
        event1.data.fd = fd;
        if (epoll_ctl(epfd_, EPOLL_CTL_ADD, fd, &event1) == -1)
        {
            LOG_INFO("epoll_ctl ADD client fd=%d failed, errno=%d", fd, errno);
            close(fd);
            continue;
        }
        LOG_INFO("New connection: fd=%d", fd);
        uint64_t conn_id = next_conn_id_.fetch_add(1);
        std::string remote_addr = formatRemoteAddr(client_addr);
        std::string connected_at = formatUtcTime(std::chrono::system_clock::now());
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            connections_.emplace(fd, Connection(fd, conn_id, remote_addr, connected_at));
        }
        business::StatsManager::getInstance().incrementConnections();
    }
}

void TcpServer::handleRead(int fd)
{
    while (true)
    {
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            if (connections_.find(fd) == connections_.end())
            {
                return;
            }
        }

        char buf[4096];
        memset(buf, 0, sizeof(buf));
        ssize_t bytes_read = recv(fd, buf, sizeof(buf), 0);
        if (bytes_read > 0)
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end())
            {
                return;
            }
            it->second.input_buffer.append(buf, bytes_read);
            business::StatsManager::getInstance().incrementReadBytes(bytes_read);
            LOG_INFO("fd=%d received %zd bytes", fd, bytes_read);
        }
        else if (bytes_read == 0)
        {
            LOG_INFO("client fd=%d disconnected", fd);
            closeConnection(fd);
            break;
        }
        else if (bytes_read == -1)
        {

            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                decodeAndEnqueue(fd);
                break;
            }
            else
            {
                perror("recv error");
                closeConnection(fd);
                break;
            }
        }
    }
}

void TcpServer::handleWrite(int fd)
{
    while (true)
    {
        std::string pending_output;
        bool should_close = false;
        bool should_switch_to_read = false;
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end())
            {
                return;
            }

            Connection &conn = it->second;
            if (conn.output_buffer.empty())
            {
                should_close = conn.closing;
                should_switch_to_read = !conn.closing;
            }
            else
            {
                pending_output = conn.output_buffer;
            }
        }

        if (should_close)
        {
            closeConnection(fd);
            return;
        }
        if (should_switch_to_read)
        {
            if (!modifyConnectionEvents(fd, EPOLLIN | EPOLLET))
            {
                return;
            }
            return;
        }

        ssize_t sent_bytes = send(fd, pending_output.data(), pending_output.size(), MSG_NOSIGNAL);
        if (sent_bytes > 0)
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(fd);
            if (it == connections_.end())
            {
                return;
            }
            Connection &conn = it->second;
            size_t erase_count = std::min(static_cast<size_t>(sent_bytes), conn.output_buffer.size());
            conn.output_buffer.erase(0, erase_count);
            business::StatsManager::getInstance().incrementWriteBytes(sent_bytes);
        }
        else if (sent_bytes == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return;
            }
            else
            {
                closeConnection(fd);
                return;
            }
        }
    }
}

void TcpServer::drainResponseQueue()
{
    Response resp;
    while (response_queue_.Try_pop(resp))
    {
        if (resp.close_connection && resp.skip_write)
        {
            bool should_close = false;
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = connections_.find(resp.fd);
                if (it == connections_.end())
                {
                    LOG_INFO("Client has disconnected.");
                    continue;
                }

                Connection &conn = it->second;
                if (conn.conn_id != resp.conn_id)
                {
                    LOG_INFO("stale response (conn_id mismatch)");
                    continue;
                }

                if (resp.mark_authenticated)
                {
                    conn.authenticated = true;
                    conn.client_id = resp.authenticated_client_id;
                }
                if (resp.type == MessageType::AUTH_RESP || resp.close_connection)
                {
                    conn.auth_pending = false;
                }
                should_close = true;
            }

            if (should_close)
            {
                closeConnection(resp.fd);
            }
            continue;
        }
        bool should_close = false;
        bool should_modify_events = false;
        RuntimeConfig config = getRuntimeConfigSnapshot();
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            auto it = connections_.find(resp.fd);
            if (it == connections_.end())
            {
                LOG_INFO("Client has disconnected.");
                continue;
            }

            Connection &conn = it->second;
            if (conn.conn_id != resp.conn_id)
            {
                LOG_INFO("stale response (conn_id mismatch)");
                continue;
            }

            if (resp.mark_authenticated)
            {
                size_t existing_count =
                    countAuthenticatedConnectionsForClientLocked(resp.authenticated_client_id, conn.fd);
                if (existing_count >= static_cast<size_t>(config.max_connections_per_client))
                {
                    business::StatsManager::getInstance().incrementErrors();
                    resp.mark_authenticated = false;
                    resp.close_connection = true;
                    resp.type = MessageType::AUTH_RESP;
                    resp.payload = R"({"allowed":false,"reason":"max connections exceeded"})";
                }
                else
                {
                    conn.authenticated = true;
                    conn.auth_pending = false;
                    conn.client_id = resp.authenticated_client_id;
                }
            }

            if (!resp.mark_authenticated &&
                (resp.type == MessageType::AUTH_RESP || resp.close_connection))
            {
                conn.auth_pending = false;
            }

            std::string encoded_data = ProtocolCodec::encode(resp);
            if (conn.output_buffer.size() + encoded_data.size() > MAX_OUT_BUFFER_SIZE)
            {
                LOG_INFO("fd=%d output buffer overflow, forcefully closing connection!", conn.fd);
                should_close = true;
            }
            else
            {
                conn.output_buffer.append(encoded_data);
                if (resp.close_connection)
                {
                    conn.closing = true;
                }
                should_modify_events = true;
            }
        }

        if (should_close)
        {
            closeConnection(resp.fd);
            continue;
        }

        if (should_modify_events && !modifyConnectionEvents(resp.fd, EPOLLIN | EPOLLOUT | EPOLLET))
        {
            continue;
        }
    }
};

bool TcpServer::decodeAndEnqueue(int fd)
{
    std::vector<Request> decoded_requests;
    std::vector<Request> requests_to_enqueue;
    std::vector<Response> out_responses;
    bool should_close = false;
    RuntimeConfig config = getRuntimeConfigSnapshot();

    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end())
        {
            return false;
        }

        Connection &conn = it->second;
        DecodeStatus status = ProtocolCodec::decode(
            conn.input_buffer,
            conn.fd,
            decoded_requests,
            conn.conn_id);
        if (status == DecodeStatus::INVALID_LENGTH)
        {
            LOG_INFO("client fd=%d sent invalid protocol, closing", conn.fd);
            should_close = true;
        }

        if (!should_close)
        {
            for (auto &req : decoded_requests)
            {
                if (!conn.authenticated)
                {
                    if (req.type != MessageType::AUTH)
                    {
                        LOG_INFO("client fd=%d sent business request before AUTH, closing", conn.fd);
                        should_close = true;
                        break;
                    }

                    if (conn.auth_pending)
                    {
                        LOG_INFO("client fd=%d sent request while AUTH is pending, closing", conn.fd);
                        should_close = true;
                        break;
                    }

                    conn.auth_pending = true;
                    requests_to_enqueue.push_back(req);
                    continue;
                }

                if (req.type == MessageType::AUTH)
                {
                    Response resp;
                    resp.fd = conn.fd;
                    resp.conn_id = conn.conn_id;
                    resp.version = req.version;
                    resp.type = MessageType::ERROR_RESP;
                    resp.request_id = req.request_id;
                    resp.status_code = 400;
                    resp.payload = R"({"status":400,"message":"already authenticated"})";
                    out_responses.push_back(resp);
                    continue;
                }

                if (!allowRequestForClientLocked(conn.client_id, config))
                {
                    business::StatsManager::getInstance().incrementErrors();
                    Response resp;
                    resp.fd = conn.fd;
                    resp.conn_id = conn.conn_id;
                    resp.version = req.version;
                    resp.type = MessageType::ERROR_RESP;
                    resp.request_id = req.request_id;
                    resp.status_code = 429;
                    resp.payload = R"({"status":429,"message":"rate limited"})";
                    out_responses.push_back(resp);
                    continue;
                }

                requests_to_enqueue.push_back(req);
            }
        }
    }

    if (should_close)
    {
        closeConnection(fd);
        return false;
    }

    for (auto &req : requests_to_enqueue)
    {
        request_queue_.push(req);
    }
    for (auto &resp : out_responses)
    {
        response_queue_.push(resp);
    }
    return true;
}

bool TcpServer::modifyConnectionEvents(int fd, uint32_t events)
{
    struct epoll_event event;
    memset(&event, 0, sizeof(event));
    event.events = events;
    event.data.fd = fd;

    if (epoll_ctl(epfd_, EPOLL_CTL_MOD, fd, &event) == -1)
    {
        LOG_INFO("epoll_ctl MOD failed fd=%d errno=%d", fd, errno);
        closeConnection(fd);
        return false;
    }

    return true;
}

void TcpServer::startMetricsReporter()
{
    metrics_reporter_ = std::thread([this]()
                                    { metricsReporterLoop(); });
}

void TcpServer::startConfigPuller()
{
    RuntimeConfig fetched;
    if (control_plane_.fetchConfig(fetched))
    {
        std::lock_guard<std::mutex> lock(runtime_config_mutex_);
        runtime_config_ = fetched;
        LOG_INFO("runtime config initialized version=%lld",
                 static_cast<long long>(runtime_config_.version));
    }
    else
    {
        LOG_INFO("runtime config fetch failed at startup, keeping default version=%lld",
                 static_cast<long long>(runtime_config_.version));
    }

    config_puller_ = std::thread([this]()
                                 { configPullerLoop(); });
}

void TcpServer::metricsReporterLoop()
{
    while (running_)
    {
        auto &stats = business::StatsManager::getInstance();
        GatewayMetrics metrics{
            gateway_id_,
            stats.getConnections(),
            stats.getTotalRequests(),
            stats.getReadBytes(),
            stats.getWriteBytes(),
            stats.getTotalErrors(),
            std::chrono::system_clock::to_time_t(std::chrono::system_clock::now())};
        control_plane_.reportMetrics(metrics);
        control_plane_.reportClients(metrics.gateway_id, buildClientSnapshot());

        for (int i = 0; i < 50 && running_; ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

void TcpServer::configPullerLoop()
{
    while (running_)
    {
        RuntimeConfig fetched;
        if (control_plane_.fetchConfig(fetched))
        {
            std::lock_guard<std::mutex> lock(runtime_config_mutex_);
            if (fetched.version > runtime_config_.version)
            {
                LOG_INFO("runtime config updated version %lld -> %lld",
                         static_cast<long long>(runtime_config_.version),
                         static_cast<long long>(fetched.version));
                runtime_config_ = fetched;
            }
        }
        else
        {
            LOG_INFO("%s", "runtime config fetch failed, keeping current config");
        }

        for (int i = 0; i < 50 && running_; ++i)
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

size_t TcpServer::countAuthenticatedConnectionsForClientLocked(const std::string &client_id, int exclude_fd) const
{
    size_t count = 0;
    for (const auto &[fd, conn] : connections_)
    {
        if (fd == exclude_fd)
        {
            continue;
        }
        if (conn.authenticated && conn.client_id == client_id)
        {
            ++count;
        }
    }
    return count;
}

bool TcpServer::allowRequestForClientLocked(const std::string &client_id, const RuntimeConfig &config)
{
    int64_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    RateLimitWindow &window = rate_limit_windows_[client_id];
    if (window.unix_second != now)
    {
        window.unix_second = now;
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

    for (const auto &[fd, conn] : connections_)
    {
        (void)fd;
        if (!conn.authenticated)
        {
            continue;
        }
        clients.push_back(ClientReport{
            conn.client_id,
            conn.remote_addr,
            conn.connected_at,
        });
    }

    return clients;
}

void TcpServer::closeConnection(int fd)
{
    bool existed = false;
    std::string client_id_to_cleanup;
    bool was_authenticated = false;
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(fd);
        if (it == connections_.end())
        {
            return;
        }
        client_id_to_cleanup = it->second.client_id;
        was_authenticated = it->second.authenticated;
        connections_.erase(it);
        if (was_authenticated && countAuthenticatedConnectionsForClientLocked(client_id_to_cleanup, -1) == 0)
        {
            rate_limit_windows_.erase(client_id_to_cleanup);
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
