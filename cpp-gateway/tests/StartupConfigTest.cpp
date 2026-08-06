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
    ScopedEnv app_environment("APP_ENV", "development");
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
        {"shutdown budget covers in-flight control-plane work", []
         {
             bool too_short = false;
             {
                 ScopedEnv control_plane_timeout("CONTROL_PLANE_TIMEOUT_MS", "1000");
                 ScopedEnv shutdown_timeout("SHUTDOWN_TIMEOUT_MS", "2099");
                 try { (void)parseStartupConfig(); }
                 catch (const std::invalid_argument &) { too_short = true; }
             }
             CHECK(too_short);

             ScopedEnv control_plane_timeout("CONTROL_PLANE_TIMEOUT_MS", "1000");
             ScopedEnv shutdown_timeout("SHUTDOWN_TIMEOUT_MS", "2100");
             CHECK_EQ(parseStartupConfig().shutdown_timeout_ms, 2100);
         }},
        {"non-development startup requires gateway secret", []
         {
             bool missing = false;
             {
                 ScopedEnv environment("APP_ENV", "production");
                 ScopedEnv token("GATEWAY_SHARED_TOKEN", "");
                 try { (void)parseStartupConfig(); }
                 catch (const std::invalid_argument &) { missing = true; }
             }
             CHECK(missing);

             ScopedEnv environment("APP_ENV", " StAgInG ");
             ScopedEnv token("GATEWAY_SHARED_TOKEN", "gateway-secret");
             CHECK_EQ(parseStartupConfig().app_environment, std::string("staging"));
         }},
        {"application environment is validated", []
         {
             ScopedEnv environment("APP_ENV", "unknown");
             bool invalid = false;
             try { (void)parseStartupConfig(); }
             catch (const std::invalid_argument &) { invalid = true; }
             CHECK(invalid);
         }},
    });
}
