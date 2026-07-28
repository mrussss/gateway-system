#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace business
{
    struct StatsSnapshot
    {
        uint64_t total_requests{};
        uint64_t errors{};
        uint64_t bytes_in{};
        uint64_t bytes_out{};
        uint64_t active_connections{};
        uint64_t request_queue_rejected{};
        uint64_t response_queue_rejected{};
        uint64_t slow_client_closed{};
        uint64_t stale_response_dropped{};
        uint64_t auth_success{};
        uint64_t auth_failure{};
    };

    class StatsManager
    {
    public:
        static StatsManager &getInstance();

        StatsManager(const StatsManager &) = delete;
        StatsManager &operator=(const StatsManager &) = delete;

        void incrementRequests();
        void incrementLogMessages();
        void incrementErrors();
        void incrementReadBytes(size_t byte_nums);
        void incrementWriteBytes(size_t byte_nums);
        void incrementConnections();
        void decrementConnections();
        void incrementRequestQueueRejected();
        void incrementResponseQueueRejected();
        void incrementSlowClientClosed();
        void incrementStaleResponseDropped();
        void incrementAuthSuccess();
        void incrementAuthFailure();
        StatsSnapshot snapshot() const;

        uint64_t getTotalRequests() const;
        uint64_t getTotalLogMessages() const;
        uint64_t getTotalErrors() const;
        uint64_t getReadBytes() const;
        uint64_t getWriteBytes() const;
        uint64_t getConnections() const;
        uint64_t getRequestQueueRejected() const;
        uint64_t getResponseQueueRejected() const;

    private:
        StatsManager() = default;
        ~StatsManager() = default;

        std::atomic<uint64_t> total_requests_{0};
        std::atomic<uint64_t> total_log_messages_{0};
        std::atomic<uint64_t> total_errors_{0};
        std::atomic<uint64_t> active_connections{0};
        std::atomic<uint64_t> total_bytes_read{0};
        std::atomic<uint64_t> total_bytes_sent{0};
        std::atomic<uint64_t> request_queue_rejected_{0};
        std::atomic<uint64_t> response_queue_rejected_{0};
        std::atomic<uint64_t> slow_client_closed_{0};
        std::atomic<uint64_t> stale_response_dropped_{0};
        std::atomic<uint64_t> auth_success_{0};
        std::atomic<uint64_t> auth_failure_{0};
    };
}
