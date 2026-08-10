#include "control_internal.hpp"

#include "gateway/acquisition.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace gateway {
namespace {

void saturating_add(std::uint64_t& target, std::uint64_t value) noexcept {
    if (value > std::numeric_limits<std::uint64_t>::max() - target) {
        target = std::numeric_limits<std::uint64_t>::max();
    } else {
        target += value;
    }
}

std::uint64_t nonnegative_nanoseconds(ControlClock::duration duration) noexcept {
    if (duration <= ControlClock::duration::zero()) {
        return 0;
    }
    const auto nanoseconds = std::chrono::duration_cast<std::chrono::nanoseconds>(
        duration).count();
    return nanoseconds <= 0 ? 0 : static_cast<std::uint64_t>(nanoseconds);
}

}  // namespace

ControlQueue::ControlQueue(std::size_t capacity) : capacity_(capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("control queue capacity must be positive");
    }
}

ControlSubmitResult ControlQueue::try_push(
    DeviceControlRequest request,
    ControlCompletion completion) {
    std::lock_guard lock(mutex_);
    if (closed_) {
        return ControlSubmitResult::Stopping;
    }
    if (queue_.size() >= capacity_) {
        ++rejected_full_;
        return ControlSubmitResult::QueueFull;
    }
    queue_.push_back(ControlOperation{
        .request = std::move(request),
        .completion = std::move(completion),
        .enqueued_at = ControlClock::now(),
    });
    max_size_ = std::max(max_size_, queue_.size());
    ++accepted_;
    not_empty_.notify_one();
    return ControlSubmitResult::Accepted;
}

bool ControlQueue::wait_pop(ControlOperation& operation) {
    std::unique_lock lock(mutex_);
    not_empty_.wait(lock, [this] { return closed_ || !queue_.empty(); });
    if (queue_.empty()) {
        return false;
    }
    operation = std::move(queue_.front());
    queue_.pop_front();
    return true;
}

void ControlQueue::close() noexcept {
    {
        std::lock_guard lock(mutex_);
        closed_ = true;
    }
    not_empty_.notify_all();
}

std::vector<ControlOperation> ControlQueue::cancel_pending() {
    std::vector<ControlOperation> cancelled;
    {
        std::lock_guard lock(mutex_);
        cancelled.reserve(queue_.size());
        while (!queue_.empty()) {
            cancelled.push_back(std::move(queue_.front()));
            queue_.pop_front();
            ++cancelled_;
        }
    }
    return cancelled;
}

bool ControlQueue::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

ControlQueueStats ControlQueue::stats() const {
    std::lock_guard lock(mutex_);
    return ControlQueueStats{
        .size = queue_.size(),
        .capacity = capacity_,
        .max_size = max_size_,
        .accepted = accepted_,
        .rejected_full = rejected_full_,
        .cancelled = cancelled_,
    };
}

DeviceControlDispatcher::DeviceControlDispatcher(
    std::size_t queue_capacity,
    ControlDriverResolver driver_resolver,
    ControlGateResolver gate_resolver)
    : queue_(queue_capacity),
      driver_resolver_(std::move(driver_resolver)),
      gate_resolver_(std::move(gate_resolver)) {
    if (!driver_resolver_) {
        throw std::invalid_argument("control dispatcher requires a driver resolver");
    }
}

DeviceControlDispatcher::~DeviceControlDispatcher() {
    stop();
}

void DeviceControlDispatcher::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (started_) {
        throw std::logic_error("control dispatcher has already been started");
    }
    if (stopping_) {
        throw std::logic_error("control dispatcher has already been stopped");
    }
    worker_ = std::jthread([this] { run(); });
    started_ = true;
}

void DeviceControlDispatcher::request_stop() noexcept {
    std::vector<ControlOperation> cancelled;
    {
        std::lock_guard lock(lifecycle_mutex_);
        if (stopping_) {
            return;
        }
        stopping_ = true;
        queue_.close();
        try {
            cancelled = queue_.cancel_pending();
        } catch (...) {
            // Allocation failure must not make a noexcept shutdown terminate
            // the process. The worker will still drain any remaining items.
            cancelled.clear();
        }
    }

    for (auto& operation : cancelled) {
        complete(operation, cancelled_result(operation.request));
    }

    {
        std::lock_guard lock(lifecycle_mutex_);
        stop_request_complete_ = true;
    }
    lifecycle_cv_.notify_all();
}

void DeviceControlDispatcher::join() {
    std::unique_lock lock(lifecycle_mutex_);
    if (!started_) {
        return;
    }

    // request_stop() may be delivering Cancelled completions on another
    // thread. Those callbacks are part of the stop barrier too.
    lifecycle_cv_.wait(lock, [this] { return stop_request_complete_; });
    while (join_in_progress_ && !joined_) {
        lifecycle_cv_.wait(lock, [this] {
            return joined_ || !join_in_progress_;
        });
    }
    if (joined_) {
        return;
    }
    join_in_progress_ = true;
    lock.unlock();

    try {
        if (worker_.joinable()) {
            worker_.join();
        }
    } catch (...) {
        lock.lock();
        join_in_progress_ = false;
        lock.unlock();
        lifecycle_cv_.notify_all();
        throw;
    }

    lock.lock();
    join_in_progress_ = false;
    joined_ = true;
    lock.unlock();
    lifecycle_cv_.notify_all();
}

void DeviceControlDispatcher::stop() noexcept {
    request_stop();
    try {
        join();
    } catch (...) {
        // Destruction and the public noexcept stop path must not leak an
        // exception from std::thread's error handling.
    }
}

ControlSubmitResult DeviceControlDispatcher::submit(
    DeviceControlRequest request,
    ControlCompletion completion) {
    {
        std::lock_guard lock(stats_mutex_);
        ++stats_.submitted;
    }

    if (!valid_request(request)) {
        std::lock_guard lock(stats_mutex_);
        ++stats_.invalid_requests;
        return ControlSubmitResult::InvalidRequest;
    }

    {
        std::lock_guard lock(lifecycle_mutex_);
        if (!started_ || stopping_) {
            std::lock_guard stats_lock(stats_mutex_);
            ++stats_.stopping;
            return ControlSubmitResult::Stopping;
        }

        // Resolve before enqueueing so a source receives an immediate and
        // deterministic UnknownDevice response instead of a delayed failure.
        IProtocolDriver* driver = nullptr;
        try {
            driver = driver_resolver_(request.device_id);
        } catch (...) {
            driver = nullptr;
        }
        if (driver == nullptr) {
            std::lock_guard stats_lock(stats_mutex_);
            ++stats_.unknown_device;
            return ControlSubmitResult::UnknownDevice;
        }

        const auto result = queue_.try_push(
            std::move(request), std::move(completion));
        std::lock_guard stats_lock(stats_mutex_);
        switch (result) {
            case ControlSubmitResult::Accepted:
                ++stats_.accepted;
                break;
            case ControlSubmitResult::QueueFull:
                ++stats_.queue_full;
                break;
            case ControlSubmitResult::UnknownDevice:
                ++stats_.unknown_device;
                break;
            case ControlSubmitResult::InvalidRequest:
                ++stats_.invalid_requests;
                break;
            case ControlSubmitResult::Stopping:
                ++stats_.stopping;
                break;
        }
        return result;
    }
}

ControlStats DeviceControlDispatcher::stats() const {
    std::lock_guard lock(stats_mutex_);
    return stats_;
}

ControlQueueStats DeviceControlDispatcher::queue_stats() const {
    return queue_.stats();
}

void DeviceControlDispatcher::run() {
    ControlOperation operation;
    while (queue_.wait_pop(operation)) {
        execute(std::move(operation));
    }
}

void DeviceControlDispatcher::execute(ControlOperation operation) {
    const auto execution_started = ControlClock::now();
    {
        std::lock_guard lock(stats_mutex_);
        saturating_add(
            stats_.queue_wait_ns,
            nonnegative_nanoseconds(execution_started - operation.enqueued_at));
    }
    auto finish = [this, &operation, execution_started](
                      DeviceControlResult result) {
        {
            std::lock_guard lock(stats_mutex_);
            saturating_add(
                stats_.execution_ns,
                nonnegative_nanoseconds(
                    ControlClock::now() - execution_started));
        }
        complete(operation, std::move(result));
    };

    auto result = DeviceControlResult{
        .request_id = operation.request.request_id,
        .status = DeviceControlStatus::Failed,
        .outputs = {},
        .message = {},
    };

    IProtocolDriver* driver = nullptr;
    try {
        driver = driver_resolver_(operation.request.device_id);
    } catch (...) {
        driver = nullptr;
    }
    if (driver == nullptr) {
        result.message = "device is no longer available";
        finish(std::move(result));
        return;
    }

    const auto now = ControlClock::now();
    if (operation.request.deadline <= now) {
        result.status = DeviceControlStatus::Timeout;
        result.message = "control request deadline expired before execution";
        finish(std::move(result));
        return;
    }

    std::unique_lock<std::timed_mutex> gate_lock;
    if (gate_resolver_) {
        std::timed_mutex* gate = nullptr;
        try {
            gate = gate_resolver_(operation.request.device_id);
        } catch (...) {
            gate = nullptr;
        }
        if (gate == nullptr) {
            result.message = "device I/O gate is unavailable";
            finish(std::move(result));
            return;
        }
        gate_lock = std::unique_lock<std::timed_mutex>{*gate, std::defer_lock};
        bool locked = false;
        try {
            if (operation.request.deadline == ControlClock::time_point::max()) {
                gate_lock.lock();
                locked = true;
            } else {
                locked = gate_lock.try_lock_until(operation.request.deadline);
            }
        } catch (...) {
            locked = false;
        }
        if (!locked) {
            result.status = DeviceControlStatus::Timeout;
            result.message = "timed out waiting for device I/O";
            finish(std::move(result));
            return;
        }
    }

    if (operation.request.deadline != ControlClock::time_point::max() &&
        ControlClock::now() >= operation.request.deadline) {
        if (gate_lock.owns_lock()) {
            gate_lock.unlock();
        }
        result.status = DeviceControlStatus::Timeout;
        result.message = "control request deadline expired before driver call";
        finish(std::move(result));
        return;
    }

    {
        std::lock_guard lock(stats_mutex_);
        ++stats_.executed;
    }
    try {
        result = driver->control(operation.request);
        // request_id is an opaque source correlation value. A driver cannot
        // replace it with a protocol-specific identifier.
        result.request_id = operation.request.request_id;
        if (ControlClock::now() > operation.request.deadline) {
            result.status = DeviceControlStatus::Timeout;
            if (result.message.empty()) {
                result.message = "control request completed after its deadline";
            }
        }
    } catch (const std::exception& error) {
        result = DeviceControlResult{
            .request_id = operation.request.request_id,
            .status = DeviceControlStatus::Failed,
            .outputs = {},
            .message = error.what(),
        };
    } catch (...) {
        result = DeviceControlResult{
            .request_id = operation.request.request_id,
            .status = DeviceControlStatus::Failed,
            .outputs = {},
            .message = "driver control failed with an unknown exception",
        };
    }

    if (gate_lock.owns_lock()) {
        gate_lock.unlock();
    }
    finish(std::move(result));
}

void DeviceControlDispatcher::complete(
    ControlOperation& operation,
    DeviceControlResult result) noexcept {
    {
        std::lock_guard lock(stats_mutex_);
        switch (result.status) {
            case DeviceControlStatus::Succeeded:
                ++stats_.succeeded;
                break;
            case DeviceControlStatus::Unsupported:
                ++stats_.unsupported;
                break;
            case DeviceControlStatus::InvalidArgument:
                ++stats_.invalid_argument;
                break;
            case DeviceControlStatus::Timeout:
                ++stats_.timeouts;
                break;
            case DeviceControlStatus::Cancelled:
                ++stats_.cancelled;
                break;
            case DeviceControlStatus::Failed:
                ++stats_.failed;
                break;
        }
    }

    if (!operation.completion) {
        return;
    }
    auto completion = std::move(operation.completion);
    try {
        completion(std::move(result));
    } catch (...) {
        // A source callback is outside the core contract; it must not kill
        // the dispatcher worker or prevent later controls from running.
    }
}

bool DeviceControlDispatcher::valid_request(
    const DeviceControlRequest& request) {
    return !request.device_id.empty() && !request.command.empty();
}

DeviceControlResult DeviceControlDispatcher::cancelled_result(
    const DeviceControlRequest& request) {
    return DeviceControlResult{
        .request_id = request.request_id,
        .status = DeviceControlStatus::Cancelled,
        .outputs = {},
        .message = "control request cancelled during shutdown",
    };
}

}  // namespace gateway
