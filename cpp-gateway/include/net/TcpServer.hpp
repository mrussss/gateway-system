#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
              std::string gateway_id, size_t request_queue_capacity = 4096,
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

    void initServer();
    void loop();
    void beginDraining();
    void finishShutdown();
    bool drainComplete();
    int epollTimeoutMs() const;
    void closeListener();
    void markReady();
    void markNotReady();

    void handleAccept();
    void handleRead(int fd);
    void handleWrite(int fd);
    void drainResponseQueue();
    void drainRejectedResponses();
    void applyResponse(Response response);
    bool enqueueWorkerResponse(Response response, ResponseProducer producer);
    void normalWorkerLoop(unsigned int worker_id);
    void authWorkerLoop(unsigned int worker_id);
    void onResponseProducerExited();
    void closeConnection(int fd);
    bool decodeAndEnqueue(int fd);
    bool modifyConnectionEvents(int fd, uint32_t events);
    uint32_t connectionEvents(bool wants_write, bool closing = false) const;

    void startMetricsReporter();
    void startConfigPuller();
    void metricsReporterLoop();
    void configPullerLoop();
    size_t countAuthenticatedConnectionsForClientLocked(const std::string &client_id,
                                                        int exclude_fd) const;
    bool allowRequestForClientLocked(const std::string &client_id,
                                     const RuntimeConfig &config);
    std::vector<ClientReport> buildClientSnapshot();

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
    std::unordered_map<int, uint64_t> rejected_response_connections_;

    std::unordered_map<int, Connection> connections_;
    mutable std::mutex connections_mutex_;
    std::vector<std::thread> workers_;
    std::vector<std::thread> auth_workers_;
    std::thread metrics_reporter_;
    std::thread config_puller_;
    ControlPlaneClient control_plane_;
    std::string gateway_id_{"gateway-001"};
    std::string gateway_boot_id_;
    int64_t process_start_time_{};
    std::string readiness_file_{"/tmp/gateway-ready"};
    RuntimeConfig runtime_config_{};
    std::mutex runtime_config_mutex_;
    std::mutex background_wait_mutex_;
    std::condition_variable background_wait_cv_;
    std::unordered_map<std::string, RateLimitWindow> rate_limit_windows_;
};
