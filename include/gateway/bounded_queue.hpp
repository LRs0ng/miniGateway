#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gateway {

enum class EnqueueResult {
    Accepted,
    Full,
    Stopping,
};

struct QueueStats {
    std::size_t size{0};
    std::size_t capacity{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected_full{0};
};

template <typename T>
class BoundedQueue {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity == 0) {
            throw std::invalid_argument("queue capacity must be positive");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    EnqueueResult try_push(T&& value) {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return EnqueueResult::Stopping;
        }
        if (queue_.size() >= capacity_) {
            ++rejected_full_;
            return EnqueueResult::Full;
        }
        queue_.push_back(std::move(value));
        ++accepted_;
        not_empty_.notify_one();
        return EnqueueResult::Accepted;
    }

    bool wait_pop(T& value) {
        std::unique_lock lock(mutex_);
        not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        value = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void close() {
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
        }
        not_empty_.notify_all();
    }

    [[nodiscard]] bool closed() const {
        std::lock_guard lock(mutex_);
        return closed_;
    }

    [[nodiscard]] QueueStats stats() const {
        std::lock_guard lock(mutex_);
        return QueueStats{
            .size = queue_.size(),
            .capacity = capacity_,
            .accepted = accepted_,
            .rejected_full = rejected_full_,
        };
    }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<T> queue_;
    bool closed_{false};
    std::uint64_t accepted_{0};
    std::uint64_t rejected_full_{0};
};

}  // namespace gateway
