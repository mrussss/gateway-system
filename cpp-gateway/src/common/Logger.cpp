#include "common/Logger.hpp"

#include <cstdarg>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>

namespace
{
std::mutex log_mutex;
std::atomic<int> configured_level{1};

int parseLevel(const std::string &level)
{
    if (level == "DEBUG") return 0;
    if (level == "INFO") return 1;
    if (level == "WARN") return 2;
    return 3;
}

void initializeLogLevel()
{
    static const bool initialized = []
    {
        const char *level = std::getenv("GATEWAY_LOG_LEVEL");
        configured_level = parseLevel(level == nullptr ? "INFO" : std::string(level));
        return true;
    }();
    (void)initialized;
}

bool debugLoggingEnabled()
{
    initializeLogLevel();
    return configured_level.load() == 0;
}

void writeLog(FILE *stream, const char *level, const char *format, va_list arguments)
{
    std::lock_guard<std::mutex> lock(log_mutex);
    std::fprintf(stream, "[%s] [Thread %zu] ", level,
                 std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::vfprintf(stream, format, arguments);
    std::fputc('\n', stream);
}
}

void setGatewayLogLevel(const std::string &level)
{
    initializeLogLevel();
    configured_level.store(parseLevel(level));
}

void gatewayLog(FILE *stream, const char *level, const char *format, ...)
{
    initializeLogLevel();
    const int message_level = std::string(level) == "INFO" ? 1 : 3;
    if (message_level < configured_level.load()) { return; }
    va_list arguments;
    va_start(arguments, format);
    writeLog(stream, level, format, arguments);
    va_end(arguments);
}

void gatewayDebugLog(const char *format, ...)
{
    if (!debugLoggingEnabled())
    {
        return;
    }
    va_list arguments;
    va_start(arguments, format);
    writeLog(stdout, "DEBUG", format, arguments);
    va_end(arguments);
}
