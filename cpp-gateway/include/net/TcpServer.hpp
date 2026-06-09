#pragma once

#include <atomic>
#include <vector>
#include <thread>
#include <unordered_map>
#include <string>
#include <mutex>
#include <sys/epoll.h>
#include "net/Connection.hpp"
#include "protocol/Request.hpp"
#include "protocol/Response.hpp"
#include "concurrent/BlockQueue.hpp"
#include "control/ControlPlaneClient.hpp"

class TcpServer
{
public:
    TcpServer(int port);
    TcpServer(int port, std::string control_plane_host, int control_plane_port);
    ~TcpServer();

    void start();
    void stop();
    static void static_sigint_handler(int sig);
    static TcpServer *getInstance()
    {
        return instance_;
    }
    size_t getRequestQueueSize()
    {
        return request_queue_.size();
    }
    size_t getResponseQueueSize()
    {
        return response_queue_.size();
    }

private:
    void initServer();
    void loop();
    void handleAccept();
    void handleRead(int fd);
    void handleWrite(int fd);
    void drainResponseQueue();
    void closeConnection(int fd);
    bool decodeAndEnqueue(Connection &conn);
    bool modifyConnectionEvents(int fd, uint32_t events);
    void startMetricsReporter();
    void metricsReporterLoop();
    std::vector<ClientReport> buildClientSnapshot();

    int port_;
    int listen_fd_;
    int epfd_;
    static TcpServer *instance_;
    std::atomic<bool> running_{false};
    std::atomic<bool> is_stopped_{false};
    std::atomic<uint64_t> next_conn_id_{1};

    BlockQueue<Request> request_queue_;
    BlockQueue<Response> response_queue_;
    std::unordered_map<int, Connection> connections_;
    std::mutex connections_mutex_;
    std::vector<std::thread> workers_;
    std::thread metrics_reporter_;
    ControlPlaneClient control_plane_;
};
