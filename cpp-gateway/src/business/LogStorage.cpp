#include "business/LogStorage.hpp"

namespace business
{
    LogStorage &LogStorage::getInstance()
    {

        static LogStorage instance;
        return instance;
    }

    LogStorage::LogStorage()
    {
        m_ofs.open("../logs/access.log", std::ios::app);
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