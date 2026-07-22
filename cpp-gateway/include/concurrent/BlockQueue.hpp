#pragma once

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <queue>
#include <utility>

enum class PushResult
{
    OK,
    FULL,
    STOPPED,
};

template <typename T>
class BlockQueue
{
public:
    explicit BlockQueue(size_t capacity) : capacity_(capacity) {}

    BlockQueue(const BlockQueue &) = delete;
    BlockQueue &operator=(const BlockQueue &) = delete;

    PushResult push(const T &item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
        {
            return PushResult::STOPPED;
        }
        if (data_queue_.size() >= capacity_)
        {
            return PushResult::FULL;
        }

        data_queue_.push(item);
        if (data_queue_.size() > peak_size_)
        {
            peak_size_ = data_queue_.size();
        }
        cv_.notify_one();
        return PushResult::OK;
    }

    PushResult push(T &&item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
        {
            return PushResult::STOPPED;
        }
        if (data_queue_.size() >= capacity_)
        {
            return PushResult::FULL;
        }

        data_queue_.push(std::move(item));
        if (data_queue_.size() > peak_size_)
        {
            peak_size_ = data_queue_.size();
        }
        cv_.notify_one();
        return PushResult::OK;
    }

    bool pop(T &item)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this]
                 { return !data_queue_.empty() || stopped_; });
        if (data_queue_.empty())
        {
            return false;
        }

        item = std::move(data_queue_.front());
        data_queue_.pop();
        return true;
    }

    bool tryPop(T &item)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (data_queue_.empty())
        {
            return false;
        }
        item = std::move(data_queue_.front());
        data_queue_.pop();
        return true;
    }

    void stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        cv_.notify_all();
    }

    size_t abort()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopped_ = true;
        const size_t discarded = data_queue_.size();
        std::queue<T> empty;
        data_queue_.swap(empty);
        cv_.notify_all();
        return discarded;
    }

    size_t size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_queue_.size();
    }

    size_t peakSize() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return peak_size_;
    }

    size_t capacity() const noexcept
    {
        return capacity_;
    }

    bool stopped() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }

private:
    const size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<T> data_queue_;
    size_t peak_size_ = 0;
    bool stopped_ = false;
};
