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
             const auto after = stats.snapshot();
             CHECK_EQ(after.slow_client_closed, before.slow_client_closed + 1);
             CHECK_EQ(after.stale_response_dropped, before.stale_response_dropped + 1);
             CHECK_EQ(after.auth_success, before.auth_success + 1);
             CHECK_EQ(after.auth_failure, before.auth_failure + 1);
         }},
    });
}
