#include "business/LogStorage.hpp"

#include <cstdlib>
#include <filesystem>

namespace business
{
    namespace
    {
        std::filesystem::path defaultLogPath()
        {
            const char *env_path = std::getenv("GATEWAY_LOG_PATH");
            if (env_path != nullptr && env_path[0] != '\0')
            {
                return env_path;
            }
            return "logs/access.log";
        }
    }

    LogStorage &LogStorage::getInstance()
    {

        static LogStorage instance;
        return instance;
    }

    LogStorage::LogStorage()
    {
        std::filesystem::path path = defaultLogPath();
        if (path.has_parent_path())
        {
            std::filesystem::create_directories(path.parent_path());
        }
        m_ofs.open(path, std::ios::app);
    }

    bool LogStorage::append(const std::string &log_line)
    {
        if (!m_ofs.is_open())
        {
            return false;
        }

        std::lock_guard<std::mutex> guard(m_mutex);
        m_ofs << log_line << "\n";
        m_ofs.flush();
        return true;
    }
}
