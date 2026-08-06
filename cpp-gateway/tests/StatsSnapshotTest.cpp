#include "TestHarness.hpp"

#include "business/StatsManager.hpp"

int main()
{
    return runTests({
        {"snapshot contains extended telemetry", []
         {
             auto &stats = business::StatsManager::getInstance();
             const auto before = stats.snapshot();
             stats.incrementSlowClientClosed();
             stats.incrementStaleResponseDropped();
             stats.incrementAuthSuccess();
             stats.incrementAuthFailure();
             stats.incrementAuthQueueRejected();
             stats.incrementAuthTaskCancelledBeforeStart();
             stats.incrementResponseQueueRejectedNormal();
             stats.incrementResponseQueueRejectedAuth();
             stats.recordAuthResult(AuthOutcome::Denied, 123);
             const auto after = stats.snapshot();
             CHECK_EQ(after.slow_client_closed, before.slow_client_closed + 1);
             CHECK_EQ(after.stale_response_dropped, before.stale_response_dropped + 1);
             CHECK_EQ(after.auth_success, before.auth_success + 1);
             CHECK_EQ(after.auth_failure, before.auth_failure + 1);
             CHECK_EQ(after.auth_queue_rejected, before.auth_queue_rejected + 1);
             CHECK_EQ(after.auth_tasks_cancelled_before_start,
                      before.auth_tasks_cancelled_before_start + 1);
             CHECK_EQ(after.response_queue_rejected_normal,
                      before.response_queue_rejected_normal + 1);
             CHECK_EQ(after.response_queue_rejected_auth,
                      before.response_queue_rejected_auth + 1);
             CHECK_EQ(after.auth_denied, before.auth_denied + 1);
             CHECK_EQ(after.auth_duration_count, before.auth_duration_count + 1);
             CHECK_EQ(after.auth_duration_total_us,
                      before.auth_duration_total_us + 123);
         }},
    });
}
