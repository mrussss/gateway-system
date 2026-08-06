#include "TestHarness.hpp"

#include <cstdlib>
#include <stdexcept>

#include "control/StartupConfig.hpp"

namespace
{
class ScopedEnv
{
public:
    ScopedEnv(const char *name, const char *value) : name_(name)
    {
        const char *old = std::getenv(name);
        if (old != nullptr)
        {
            old_ = old;
            had_old_ = true;
        }
        setenv(name, value, 1);
    }
    ~ScopedEnv()
    {
        if (had_old_) { setenv(name_.c_str(), old_.c_str(), 1); }
        else { unsetenv(name_.c_str()); }
    }

private:
    std::string name_;
    std::string old_;
    bool had_old_ = false;
};
} // namespace

int main()
{
    return runTests({
        {"new auth and control-plane settings parse", []
         {
             ScopedEnv timeout("CONTROL_PLANE_TIMEOUT_MS", "750");
             ScopedEnv workers("AUTH_WORKER_COUNT", "3");
             ScopedEnv capacity("AUTH_QUEUE_CAPACITY", "64");
             const StartupConfig config = parseStartupConfig();
             CHECK_EQ(config.control_plane_timeout_ms, 750);
             CHECK_EQ(config.auth_worker_count, 3U);
             CHECK_EQ(config.auth_queue_capacity, size_t{64});
         }},
        {"malformed or out-of-range settings fail startup", []
         {
             bool malformed = false;
             {
                 ScopedEnv value("CONTROL_PLANE_TIMEOUT_MS", "1000ms");
                 try { (void)parseStartupConfig(); }
                 catch (const std::invalid_argument &) { malformed = true; }
             }
             bool out_of_range = false;
             {
                 ScopedEnv value("AUTH_WORKER_COUNT", "0");
                 try { (void)parseStartupConfig(); }
                 catch (const std::invalid_argument &) { out_of_range = true; }
             }
             CHECK(malformed);
             CHECK(out_of_range);
         }},
    });
}
