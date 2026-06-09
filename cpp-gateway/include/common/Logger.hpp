#pragma once
#include <iostream>
#include <thread>
#include <cstdio>
#include <functional>
#include <mutex>

inline std::mutex g_log_mutex;

// variadic template logging
template <typename... Args>
inline void LOG_INFO(const char *fmt, Args... args)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    printf("[INFO] [Thread %zu] ", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if constexpr (sizeof...(Args) == 0)
    {
        printf("%s", fmt);
    }
    else
    {
        printf(fmt, args...);
    }
    printf("\n");
}

template <typename... Args>
inline void LOG_ERROR(const char *fmt, Args... args)
{
    std::lock_guard<std::mutex> lock(g_log_mutex);
    fprintf(stderr, "[ERROR] [Thread %zu] ", std::hash<std::thread::id>{}(std::this_thread::get_id()));
    if constexpr (sizeof...(Args) == 0)
    {
        fprintf(stderr, "%s", fmt);
    }
    else
    {
        fprintf(stderr, fmt, args...);
    }
    fprintf(stderr, "\n");
}
