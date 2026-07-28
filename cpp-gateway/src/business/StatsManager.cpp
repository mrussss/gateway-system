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

    void StatsManager::incrementRequestQueueRejected()
    {
        request_queue_rejected_++;
    }

    void StatsManager::incrementResponseQueueRejected()
    {
        response_queue_rejected_++;
    }

    uint64_t StatsManager::getRequestQueueRejected() const
    {
        return request_queue_rejected_.load();
    }

    uint64_t StatsManager::getResponseQueueRejected() const
    {
        return response_queue_rejected_.load();
    }

    void StatsManager::incrementSlowClientClosed() { slow_client_closed_++; }
    void StatsManager::incrementStaleResponseDropped() { stale_response_dropped_++; }
    void StatsManager::incrementAuthSuccess() { auth_success_++; }
    void StatsManager::incrementAuthFailure() { auth_failure_++; }

    StatsSnapshot StatsManager::snapshot() const
    {
        return {total_requests_.load(), total_errors_.load(), total_bytes_read.load(),
                total_bytes_sent.load(), active_connections.load(),
                request_queue_rejected_.load(), response_queue_rejected_.load(),
                slow_client_closed_.load(), stale_response_dropped_.load(),
                auth_success_.load(), auth_failure_.load()};
    }

}
