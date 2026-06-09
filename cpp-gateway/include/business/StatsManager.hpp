#pragma once
#include <atomic>
#include <cstdint>
#include <cstddef>

namespace business
{
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

        uint64_t getTotalRequests() const;
        uint64_t getTotalLogMessages() const;
        uint64_t getTotalErrors() const;
        uint64_t getReadBytes() const;
        uint64_t getWriteBytes() const;
        uint64_t getConnections() const;

    private:
        StatsManager() = default;
        ~StatsManager() = default;

        std::atomic<uint64_t> total_requests_{0};
        std::atomic<uint64_t> total_log_messages_{0};
        std::atomic<uint64_t> total_errors_{0};
        std::atomic<uint64_t> active_connections{0};
        std::atomic<uint64_t> total_bytes_read{0};
        std::atomic<uint64_t> total_bytes_sent{0};
    };
}