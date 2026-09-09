#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "business/AuthTask.hpp"
#include "concurrent/BlockQueue.hpp"
#include "control/ControlPlaneClient.hpp"
#include "net/Connection.hpp"
#include "net/ReactorNotifier.hpp"
#include "protocol/Request.hpp"
#include "protocol/Response.hpp"

enum class ServerState
{
    RUNNING,
    DRAINING,
    STOPPED,
};

class TcpServer
{
public:
    explicit TcpServer(int port);
    TcpServer(int port, std::string control_plane_host, int control_plane_port);
    TcpServer(int port, std::string control_plane_host, int control_plane_port,
              size_t request_queue_capacity,
              size_t response_queue_capacity = 4096, int shutdown_timeout_ms = 5000,
              unsigned int worker_count = 0, std::string gateway_token = "",
              int control_plane_timeout_ms = 1000,
              unsigned int auth_worker_count = 2,
              size_t auth_queue_capacity = 32);
    ~TcpServer();

    TcpServer(const TcpServer &) = delete;
    TcpServer &operator=(const TcpServer &) = delete;

    void start();
    void stop();
    static void staticSignalHandler(int signal_number);
    static TcpServer *getInstance() { return instance_; }

    size_t getRequestQueueSize() const { return request_queue_.size(); }
    size_t getResponseQueueSize() const { return response_queue_.size(); }
    size_t getAuthQueueSize() const { return auth_queue_.size(); }
    size_t getRequestQueueCapacity() const { return request_queue_.capacity(); }
    size_t getResponseQueueCapacity() const { return response_queue_.capacity(); }
    size_t getAuthQueueCapacity() const { return auth_queue_.capacity(); }
    size_t getRequestQueuePeakSize() const { return request_queue_.peakSize(); }
    size_t getResponseQueuePeakSize() const { return response_queue_.peakSize(); }
    size_t getAuthQueuePeakSize() const { return auth_queue_.peakSize(); }
    size_t getAuthInFlight() const { return auth_in_flight_.load(std::memory_order_relaxed); }
    RuntimeConfig getRuntimeConfigSnapshot();
    ServerState state() const noexcept { return state_.load(); }

private:
    enum class ResponseProducer
    {
        NormalWorker,
        AuthWorker,
    };

    enum class ResponseOrigin
    {
        Local,
        Worker,
    };

    struct ConnectionKey
    {
        int fd;
        uint64_t conn_id;

        bool operator==(const ConnectionKey &other) const noexcept
        {
            return fd == other.fd && conn_id == other.conn_id;
        }
    };

    struct ConnectionKeyHash
    {
        size_t operator()(const ConnectionKey &key) const noexcept
        {
            const size_t fd_hash = std::hash<int>{}(key.fd);
            const size_t conn_id_hash = std::hash<uint64_t>{}(key.conn_id);
            return fd_hash ^ (conn_id_hash + static_cast<size_t>(0x9e3779b9) +
                              (fd_hash << 6) + (fd_hash >> 2));
        }
    };

    void initServer();
    void loop();
    void beginDraining();
    void finishShutdown();
    bool drainComplete();
    int epollTimeoutMs() const;
    ControlPlaneClient::Deadline shutdownDeadline() const;
    void closeListener();
    void markReady();
    void markNotReady();

    void handleAccept();
    void handleRead(int fd);
    void handleWrite(int fd);
    void markReadEof(int fd);
    void maybeCloseAfterDrain(int fd);
    void drainResponseQueue();
    void drainRejectedResponses();
    void applyResponse(Response response, ResponseOrigin origin);
    bool enqueueWorkerResponse(Response response, ResponseProducer producer);
    void normalWorkerLoop(unsigned int worker_id);
    void authWorkerLoop(unsigned int worker_id);
    void onResponseProducerExited();
    void closeConnection(int fd);
    bool decodeAndEnqueue(int fd, bool read_eof = false);
    bool modifyConnectionEvents(int fd, uint32_t events);
    uint32_t connectionEvents(const Connection &connection) const;
    bool canCloseAfterDrain(const Connection &connection) const;
    void assertReactorThread() const;

    void startConfigPuller();
    void configPullerLoop();
    size_t countAuthenticatedConnectionsForClient(const std::string &client_id,
                                                  int exclude_fd) const;
    bool allowRequestForClient(const std::string &client_id, const RuntimeConfig &config);

    struct RateLimitWindow
    {
        std::chrono::steady_clock::time_point started_at{};
        int count = 0;
    };

    int port_;
    int listen_fd_ = -1;
    int epfd_ = -1;
    static TcpServer *instance_;
    static volatile std::sig_atomic_t signal_stop_requested_;

    std::atomic<ServerState> state_{ServerState::STOPPED};
    std::atomic<bool> started_{false};
    std::atomic<bool> shutdown_finished_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<unsigned int> response_producers_remaining_{0};
    std::atomic<size_t> auth_in_flight_{0};
    std::thread::id loop_thread_id_{};
    std::chrono::steady_clock::time_point shutdown_deadline_{};
    mutable std::mutex shutdown_deadline_mutex_;
    const std::chrono::milliseconds shutdown_timeout_;
    const unsigned int configured_worker_count_;
    const unsigned int configured_auth_worker_count_;
    std::mutex stopped_mutex_;
    std::condition_variable stopped_cv_;

    std::atomic<uint64_t> next_conn_id_{1};
    BlockQueue<Request> request_queue_;
    BlockQueue<AuthTask> auth_queue_;
    BlockQueue<Response> response_queue_;
    ReactorNotifier notifier_;
    std::mutex rejected_responses_mutex_;
    std::unordered_set<ConnectionKey, ConnectionKeyHash> rejected_response_connections_;

    std::unordered_map<int, Connection> connections_;
    std::vector<std::thread> workers_;
    std::vector<std::thread> auth_workers_;
    std::thread config_puller_;
    ControlPlaneClient control_plane_;
    std::string readiness_file_{"/tmp/gateway-ready"};
    RuntimeConfig runtime_config_{};
    std::mutex runtime_config_mutex_;
    std::mutex background_wait_mutex_;
    std::condition_variable background_wait_cv_;
    std::unordered_map<std::string, RateLimitWindow> rate_limit_windows_;
};
