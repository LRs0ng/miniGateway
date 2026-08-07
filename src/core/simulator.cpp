#include "gateway/simulator.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace gateway {
namespace {

bool has_point(const DeviceConfig& device, std::string_view name) {
    return std::any_of(
        device.points.begin(), device.points.end(),
        [name](const PointConfig& point) { return point.name == name; });
}

double simulated_value(std::string_view point, std::uint64_t sequence) {
    if (point.find("temperature") != std::string_view::npos) {
        return 78.0 + static_cast<double>(sequence % 8) * 2.0;
    }
    if (point.find("pressure") != std::string_view::npos) {
        return 100.0 + static_cast<double>(sequence % 5) * 0.5;
    }
    if (point.find("vibration_x") != std::string_view::npos) {
        return 0.2 + static_cast<double>(sequence % 7) * 0.1;
    }
    if (point.find("vibration_y") != std::string_view::npos) {
        return 0.4 + static_cast<double>(sequence % 5) * 0.12;
    }
    return static_cast<double>(sequence);
}

RawBatch timeout_batch(const CompiledPlan& plan) {
    RawBatch batch{
        .device_id = plan.device_id,
        .source = "simulator_poll",
        .samples = {},
    };
    const auto timestamp = unix_time_ns();
    for (const auto& point : plan.points) {
        batch.samples.push_back(RawSample{
            .point = point,
            .value = 0.0,
            .status = SampleStatus::Timeout,
            .source_time_ns = timestamp,
        });
    }
    return batch;
}

}  // namespace

PollSimulatorDriver::PollSimulatorDriver(std::chrono::milliseconds latency)
    : latency_(latency) {
    if (latency < std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("simulator latency cannot be negative");
    }
}

DriverCapabilities PollSimulatorDriver::capabilities() const {
    return DriverCapabilities{.mode = AcquisitionMode::Poll};
}

void PollSimulatorDriver::configure(const DeviceConfig& device, SampleSink) {
    if (started_) {
        throw std::logic_error("cannot configure a running poll simulator");
    }
    device_ = device;
    configured_ = true;
}

void PollSimulatorDriver::start() {
    if (!configured_) {
        throw std::logic_error("poll simulator is not configured");
    }
    started_ = true;
}

void PollSimulatorDriver::stop() noexcept {
    started_ = false;
}

CompiledPlan PollSimulatorDriver::compile(const CollectionGroup& group) {
    if (!configured_ || group.device_id != device_.id) {
        throw std::invalid_argument("group does not belong to simulator device");
    }
    for (const auto& point : group.points) {
        if (!has_point(device_, point)) {
            throw std::invalid_argument("unknown point in group: " + point);
        }
    }
    return CompiledPlan{
        .group_id = group.id,
        .device_id = group.device_id,
        .points = group.points,
    };
}

RawBatch PollSimulatorDriver::poll(const CompiledPlan& plan, TimePoint deadline) {
    if (!started_) {
        throw std::logic_error("poll simulator is stopped");
    }

    const auto now = SchedulerClock::now();
    if (now >= deadline || latency_ > deadline - now) {
        return timeout_batch(plan);
    }
    std::this_thread::sleep_for(latency_);

    ++sequence_;
    RawBatch batch{
        .device_id = plan.device_id,
        .source = "simulator_poll",
        .samples = {},
    };
    const auto timestamp = unix_time_ns();
    for (const auto& point : plan.points) {
        batch.samples.push_back(RawSample{
            .point = point,
            .value = simulated_value(point, sequence_),
            .status = SampleStatus::Good,
            .source_time_ns = timestamp,
        });
    }
    return batch;
}

PushSimulatorDriver::PushSimulatorDriver(std::chrono::milliseconds interval)
    : interval_(interval) {
    if (interval <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("push interval must be positive");
    }
}

PushSimulatorDriver::~PushSimulatorDriver() {
    stop();
}

DriverCapabilities PushSimulatorDriver::capabilities() const {
    return DriverCapabilities{.mode = AcquisitionMode::Push};
}

void PushSimulatorDriver::configure(const DeviceConfig& device, SampleSink sink) {
    if (worker_.joinable()) {
        throw std::logic_error("cannot configure a running push simulator");
    }
    if (!sink) {
        throw std::invalid_argument("push simulator requires a sample sink");
    }
    device_ = device;
    sink_ = std::move(sink);
    configured_ = true;
}

void PushSimulatorDriver::start() {
    if (!configured_) {
        throw std::logic_error("push simulator is not configured");
    }
    if (worker_.joinable()) {
        throw std::logic_error("push simulator is already running");
    }
    worker_ = std::jthread([this](std::stop_token token) { run(token); });
}

void PushSimulatorDriver::stop() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    worker_.request_stop();
    wakeup_.notify_all();
    worker_.join();
}

PushSimulatorStats PushSimulatorDriver::stats() const {
    return PushSimulatorStats{
        .produced = produced_.load(std::memory_order_relaxed),
        .queue_full = queue_full_.load(std::memory_order_relaxed),
    };
}

void PushSimulatorDriver::run(std::stop_token stop_token) {
    std::unique_lock lock(mutex_);
    while (!stop_token.stop_requested()) {
        const auto interrupted = wakeup_.wait_for(
            lock, interval_, [&stop_token] { return stop_token.stop_requested(); });
        if (interrupted) {
            break;
        }

        lock.unlock();
        auto batch = make_batch();
        const auto result = sink_(std::move(batch));
        produced_.fetch_add(1, std::memory_order_relaxed);
        if (result == EnqueueResult::Full) {
            queue_full_.fetch_add(1, std::memory_order_relaxed);
        }
        if (result == EnqueueResult::Stopping) {
            return;
        }
        lock.lock();
    }
}

RawBatch PushSimulatorDriver::make_batch() {
    ++sequence_;
    RawBatch batch{
        .device_id = device_.id,
        .source = "simulator_push",
        .samples = {},
    };
    const auto timestamp = unix_time_ns();
    for (const auto& point : device_.points) {
        batch.samples.push_back(RawSample{
            .point = point.name,
            .value = simulated_value(point.name, sequence_),
            .status = SampleStatus::Good,
            .source_time_ns = timestamp,
        });
    }
    return batch;
}

}  // namespace gateway
