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

    void StatsManager::incrementResponseQueueRejectedNormal()
    {
        response_queue_rejected_normal_++;
        incrementResponseQueueRejected();
    }

    void StatsManager::incrementResponseQueueRejectedAuth()
    {
        response_queue_rejected_auth_++;
        incrementResponseQueueRejected();
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
    void StatsManager::incrementAuthQueueRejected() { auth_queue_rejected_++; }
    void StatsManager::incrementAuthTaskCancelledBeforeStart()
    {
        auth_tasks_cancelled_before_start_++;
    }

    void StatsManager::recordAuthResult(AuthOutcome outcome, uint64_t duration_us)
    {
        switch (outcome)
        {
        case AuthOutcome::Allowed:
            auth_allowed_++;
            break;
        case AuthOutcome::Denied:
            auth_denied_++;
            break;
        case AuthOutcome::Unavailable:
            auth_unavailable_++;
            break;
        }
        auth_duration_count_++;
        auth_duration_total_us_.fetch_add(duration_us);
    }

    StatsSnapshot StatsManager::snapshot() const
    {
        StatsSnapshot result;
        result.total_requests = total_requests_.load();
        result.errors = total_errors_.load();
        result.bytes_in = total_bytes_read.load();
        result.bytes_out = total_bytes_sent.load();
        result.active_connections = active_connections.load();
        result.request_queue_rejected = request_queue_rejected_.load();
        result.response_queue_rejected = response_queue_rejected_.load();
        result.response_queue_rejected_normal = response_queue_rejected_normal_.load();
        result.response_queue_rejected_auth = response_queue_rejected_auth_.load();
        result.slow_client_closed = slow_client_closed_.load();
        result.stale_response_dropped = stale_response_dropped_.load();
        result.auth_success = auth_success_.load();
        result.auth_failure = auth_failure_.load();
        result.auth_queue_rejected = auth_queue_rejected_.load();
        result.auth_tasks_cancelled_before_start =
            auth_tasks_cancelled_before_start_.load();
        result.auth_allowed = auth_allowed_.load();
        result.auth_denied = auth_denied_.load();
        result.auth_unavailable = auth_unavailable_.load();
        result.auth_duration_count = auth_duration_count_.load();
        result.auth_duration_total_us = auth_duration_total_us_.load();
        return result;
    }

}
