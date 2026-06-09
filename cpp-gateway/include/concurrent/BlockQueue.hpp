#pragma once
#include <queue>
#include <mutex>
#include <condition_variable>
#include <utility>

template <typename T>
class BlockQueue
{
private:
    std::queue<T> data_queue;
    std::mutex mtx;
    std::condition_variable cv_;
    bool _is_stopped = false;

public:
    bool push(const T &item)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (_is_stopped == true)
        {
            return false;
        }

        data_queue.push(item);
        cv_.notify_one();
        return true;
    }
    bool pop(T &item)
    {
        std::unique_lock<std::mutex> lock(mtx);
        cv_.wait(lock, [this]
                 { return !data_queue.empty() || _is_stopped; });
        if (data_queue.empty() && _is_stopped)
        {
            return false;
        }
        item = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }

    bool Try_pop(T &item)
    {
        std::lock_guard<std::mutex> lock(mtx);
        if (data_queue.empty())
        {
            return false;
        }
        item = std::move(data_queue.front());
        data_queue.pop();
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mtx);
        _is_stopped = true;
        cv_.notify_all();
    }
    size_t size()
    {
        std::lock_guard<std::mutex> lock(mtx);
        return data_queue.size();
    }
};
