#include "simulator.hpp"

#include "plugin_support/plugin_json.hpp"
#include "gateway/plugin_api.hpp"
#include "simulation_support.hpp"

#include <iostream>
#include <stdexcept>
#include <syncstream>
#include <utility>

namespace gateway {

namespace {

std::unique_ptr<PushSimulatorDriver> make_push_simulator(
    std::string_view settings_json) {
    constexpr std::string_view plugin{"push simulator"};
    const auto settings =
        plugin_json::parse_object(settings_json, plugin);
    return std::make_unique<PushSimulatorDriver>(
        plugin_json::milliseconds_member(settings, "interval_ms", plugin, true));
}

}  // namespace

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

DeviceControlResult PushSimulatorDriver::control(
    const DeviceControlRequest& request) {
    if (!worker_.joinable()) {
        throw std::logic_error("push simulator is stopped");
    }
    if (request.device_id != device_.id) {
        return DeviceControlResult{
            .request_id = request.request_id,
            .status = DeviceControlStatus::InvalidArgument,
            .outputs = {},
            .message = "control request belongs to another device",
        };
    }

    std::osyncstream{std::cout}
        << "[push_simulator] control request_id=" << request.request_id
        << " device=" << request.device_id
        << " command=" << request.command << '\n';
    return DeviceControlResult{
        .request_id = request.request_id,
        .status = DeviceControlStatus::Succeeded,
        .outputs = {},
        .message = "control command printed",
    };
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
            lock,
            interval_,
            [&stop_token] { return stop_token.stop_requested(); });
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
            .value = simulation_support::simulated_value(point.name, sequence_),
            .status = Quality::Good,
            .source_time_ns = timestamp,
        });
    }
    return batch;
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return make_push_simulator(
                   settings_json == nullptr ? std::string_view{"{}"}
                                             : std::string_view{settings_json})
            .release();
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<PushSimulatorDriver*>(plugin);
}

}  // namespace gateway
