#include "common/Logger.hpp"

#include <cstdarg>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace
{
std::mutex log_mutex;

bool debugLoggingEnabled()
{
    static const bool enabled = []
    {
        const char *level = std::getenv("GATEWAY_LOG_LEVEL");
        return level != nullptr && std::string(level) == "DEBUG";
    }();
    return enabled;
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

void gatewayLog(FILE *stream, const char *level, const char *format, ...)
{
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
