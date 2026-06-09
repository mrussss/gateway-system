#pragma once
#include <fstream>
#include <mutex>
#include <string>

namespace business
{
    class LogStorage
    {
    public:
        static LogStorage &getInstance();
        LogStorage(const LogStorage &) = delete;
        LogStorage &operator=(const LogStorage &) = delete;

        bool append(const std::string &log_line);

    private:
        LogStorage();
        ~LogStorage() = default;
        std::mutex m_mutex;
        std::ofstream m_ofs;
    };
}