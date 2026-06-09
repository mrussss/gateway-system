#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>
#include <atomic>
#include <csignal>
#include <cerrno>
#include "concurrent/BlockQueue.hpp"
#include "net/TcpServer.hpp"
#include "net/SocketUtil.hpp"
#include "protocol/Request.hpp"
#include "protocol/ProtocolCodec.hpp"
#include "business/Dispatcher.hpp"
#include "business/StatsManager.hpp"
#include "common/Logger.hpp"

namespace
{
    constexpr size_t MAX_OUT_BUFFER_SIZE = 2 * 1024 * 1024;
}

// =========================================================
// 1. Construction & Destruction
// =========================================================
TcpServer::TcpServer(int port) : port_(port), listen_fd_(-1), epfd_(-1), running_(false) {}

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
                                      business::Dispatcher Dispatch;
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
catch(const std::exception& e)
{
   std::cerr << "failed to push response: " << e.what() << std::endl;
}
catch(...){
    std::cerr << "unknown error pushing response!" << std::endl;
}

                                         

                                      }
                                  }
                                  catch (const std::exception &e)
                                  {
                                      std::cerr << "worker thread failed: " << e.what() << std::endl;
                                  }
                                catch(...){
std::cerr << "worker thread unknown error!" << std::endl;
                                } });
    }
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
    for (const auto &[fd, conn] : connections_)
    {
        fds_to_close.push_back(fd);
    }
    for (int fd : fds_to_close)
    {
        closeConnection(fd);
    }
    request_queue_.stop();
    response_queue_.stop();
    LOG_INFO("task queue stopped, waiting for all workers to exit");

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
        connections_.insert({fd, Connection(fd, conn_id)});
        business::StatsManager::getInstance().incrementConnections();
    }
}

void TcpServer::handleRead(int fd)
{
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return;
    Connection &conn = it->second;
    while (true)
    {
        char buf[4096];
        memset(buf, 0, sizeof(buf));
        ssize_t bytes_read = recv(fd, buf, sizeof(buf), 0);
        if (bytes_read > 0)
        {
            conn.input_buffer.append(buf, bytes_read);
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
                decodeAndEnqueue(conn);
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
    auto it = connections_.find(fd);
    if (it == connections_.end())
        return;
    Connection &conn = it->second;
    while (!conn.output_buffer.empty())
    {
        ssize_t sent_bytes = send(fd, conn.output_buffer.data(), conn.output_buffer.size(), MSG_NOSIGNAL);
        if (sent_bytes > 0)
        {
            conn.output_buffer.erase(0, sent_bytes);
            business::StatsManager::getInstance().incrementWriteBytes(sent_bytes);
        }
        else if (sent_bytes == -1)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            else
            {
                closeConnection(fd);
                return;
            }
        }
        if (conn.output_buffer.empty())
        {
            if (!modifyConnectionEvents(fd, EPOLLIN | EPOLLET))
            {
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

        std::string encoded_data = ProtocolCodec::encode(resp);
        if (conn.output_buffer.size() + encoded_data.size() > MAX_OUT_BUFFER_SIZE)
        {
            LOG_INFO("fd=%d output buffer overflow, forcefully closing connection!", conn.fd);
            closeConnection(conn.fd);
            continue;
        }

        conn.output_buffer.append(encoded_data);
        if (!modifyConnectionEvents(resp.fd, EPOLLIN | EPOLLOUT | EPOLLET))
        {
            continue;
        }
    }
};

bool TcpServer::decodeAndEnqueue(Connection &conn)
{
    std::vector<Request> out_requests;
    DecodeStatus status = ProtocolCodec::decode(
        conn.input_buffer,
        conn.fd,
        out_requests,
        conn.conn_id);
    if (status == DecodeStatus::INVALID_LENGTH)
    {
        LOG_INFO("client fd=%d sent invalid protocol, closing", conn.fd);
        closeConnection(conn.fd);
        return false;
    }
    for (auto &req : out_requests)
    {
        request_queue_.push(req);
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

void TcpServer::closeConnection(int fd)
{
    auto it = connections_.find(fd);
    if (it == connections_.end())
    {
        return;
    }

    if (epfd_ != -1)
    {
        epoll_ctl(epfd_, EPOLL_CTL_DEL, fd, nullptr);
    }

    close(fd);
    connections_.erase(it);
    business::StatsManager::getInstance().decrementConnections();
}