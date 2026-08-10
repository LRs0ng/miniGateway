#pragma once

#include "gateway/control.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace gateway {

class IProtocolDriver;

struct ControlOperation {
    DeviceControlRequest request;
    ControlCompletion completion;
    ControlClock::time_point enqueued_at{};
};

// Control requests have separate capacity, shutdown, and completion semantics
// from the raw-sample queue.
class ControlQueue final {
public:
    explicit ControlQueue(std::size_t capacity);

    ControlQueue(const ControlQueue&) = delete;
    ControlQueue& operator=(const ControlQueue&) = delete;

    ControlSubmitResult try_push(
        DeviceControlRequest request,
        ControlCompletion completion);
    bool wait_pop(ControlOperation& operation);
    void close() noexcept;

    // The caller delivers cancellation completions outside the queue lock.
    [[nodiscard]] std::vector<ControlOperation> cancel_pending();

    [[nodiscard]] bool closed() const;
    [[nodiscard]] ControlQueueStats stats() const;

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable not_empty_;
    std::deque<ControlOperation> queue_;
    bool closed_{false};
    std::size_t max_size_{0};
    std::uint64_t accepted_{0};
    std::uint64_t rejected_full_{0};
    std::uint64_t cancelled_{0};
};

using ControlDriverResolver =
    std::function<IProtocolDriver*(std::string_view device_id)>;
using ControlGateResolver =
    std::function<std::timed_mutex*(std::string_view device_id)>;

// Runtime's concrete control worker. This is deliberately not plugin API.
class DeviceControlDispatcher final {
public:
    DeviceControlDispatcher(
        std::size_t queue_capacity,
        ControlDriverResolver driver_resolver,
        ControlGateResolver gate_resolver = {});
    ~DeviceControlDispatcher();

    DeviceControlDispatcher(const DeviceControlDispatcher&) = delete;
    DeviceControlDispatcher& operator=(const DeviceControlDispatcher&) = delete;

    void start();
    void request_stop() noexcept;
    void join();
    void stop() noexcept;

    [[nodiscard]] ControlSubmitResult submit(
        DeviceControlRequest request,
        ControlCompletion completion = {});
    [[nodiscard]] ControlStats stats() const;
    [[nodiscard]] ControlQueueStats queue_stats() const;

private:
    void run();
    void execute(ControlOperation operation);
    void complete(
        ControlOperation& operation,
        DeviceControlResult result) noexcept;
    [[nodiscard]] static bool valid_request(const DeviceControlRequest& request);
    [[nodiscard]] static DeviceControlResult cancelled_result(
        const DeviceControlRequest& request);

    ControlQueue queue_;
    ControlDriverResolver driver_resolver_;
    ControlGateResolver gate_resolver_;

    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    std::jthread worker_;
    bool started_{false};
    bool stopping_{false};
    bool stop_request_complete_{false};
    bool join_in_progress_{false};
    bool joined_{false};

    mutable std::mutex stats_mutex_;
    ControlStats stats_{};
};

}  // namespace gateway
