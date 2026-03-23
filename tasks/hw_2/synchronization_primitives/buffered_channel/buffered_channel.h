#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

template <class T>
class BufferedChannel {
public:
    explicit BufferedChannel(int size) : capacity_(size) {
    }

    void Send(const T& value) {
        std::unique_lock lock(mutex_);
        can_send_.wait(lock, [this]() {
            return closed_ || queue_.size() < static_cast<size_t>(capacity_);
        });
        if (closed_) {
            throw std::runtime_error("Channel is closed");
        }
        queue_.push_back(value);
        can_recv_.notify_one();
    }

    std::optional<T> Recv() {
        std::unique_lock lock(mutex_);
        can_recv_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop_front();
        can_send_.notify_one();
        return value;
    }

    void Close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        can_send_.notify_all();
        can_recv_.notify_all();
    }

private:
    int capacity_;
    bool closed_ = false;
    std::deque<T> queue_;
    std::mutex mutex_;
    std::condition_variable can_send_;
    std::condition_variable can_recv_;
};
