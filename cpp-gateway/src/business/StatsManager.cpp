#include "business/StatsManager.hpp"

namespace business
{
    StatsManager &StatsManager::getInstance()
    {
        static StatsManager instance;
        return instance;
    }

    void StatsManager::incrementRequests()
    {
        total_requests_++;
    }

    uint64_t StatsManager::getTotalRequests() const
    {
        return total_requests_.load();
    }

    void StatsManager::incrementLogMessages()
    {
        total_log_messages_++;
    }

    uint64_t StatsManager::getTotalLogMessages() const
    {
        return total_log_messages_.load();
    }

    void StatsManager::incrementErrors()
    {
        total_errors_++;
    }

    uint64_t StatsManager::getTotalErrors() const
    {
        return total_errors_.load();
    }
    void StatsManager::incrementReadBytes(size_t byte_nums)
    {
        total_bytes_read.fetch_add(byte_nums);
    }

    uint64_t StatsManager::getReadBytes() const
    {
        return total_bytes_read.load();
    }

    void StatsManager::incrementWriteBytes(size_t byte_nums)
    {
        total_bytes_sent.fetch_add(byte_nums);
    }

    uint64_t StatsManager::getWriteBytes() const
    {
        return total_bytes_sent.load();
    }

    void StatsManager::incrementConnections()
    {
        active_connections++;
    }

    void StatsManager::decrementConnections()
    {
        uint64_t current = active_connections.load();
        while (current > 0)
        {
            if (active_connections.compare_exchange_weak(current, current - 1))
            {
                return;
            }
        }
    }

    uint64_t StatsManager::getConnections() const
    {
        return active_connections.load();
    }

}
