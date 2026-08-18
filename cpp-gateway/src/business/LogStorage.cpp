#include "business/LogStorage.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace business
{
    LogStorage &LogStorage::getInstance()
    {

        static LogStorage instance;
        return instance;
    }

    LogStorage::LogStorage()
    {
        const char *env_path = std::getenv("GATEWAY_LOG_PATH");
        if (env_path == nullptr || env_path[0] == '\0')
        {
            m_use_stdout = true;
            return;
        }

        try
        {
            const std::filesystem::path path(env_path);
            if (path.has_parent_path())
            {
                std::filesystem::create_directories(path.parent_path());
            }
            m_ofs.open(path, std::ios::app);
        }
        catch (const std::filesystem::filesystem_error &)
        {
            // append() returns false so the request receives an explicit failure.
        }
    }

    bool LogStorage::append(const std::string &log_line)
    {
        std::lock_guard<std::mutex> guard(m_mutex);
        if (m_use_stdout)
        {
            std::cout << log_line << '\n';
            std::cout.flush();
            return static_cast<bool>(std::cout);
        }
        if (!m_ofs.is_open())
        {
            return false;
        }

        m_ofs << log_line << "\n";
        m_ofs.flush();
        return static_cast<bool>(m_ofs);
    }
}
