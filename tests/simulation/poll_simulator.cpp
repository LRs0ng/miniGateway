#include "simulator.hpp"

#include "plugin_support/plugin_json.hpp"
#include "simulation_support.hpp"

#include <stdexcept>
#include <utility>

namespace gateway {
namespace {

RawBatch timeout_batch(const CollectionGroup& group) {
    RawBatch batch{
        .device_id = group.device_id,
        .source = "simulator_poll",
        .samples = {},
    };
    const auto timestamp = unix_time_ns();
    for (const auto& point : group.points) {
        batch.samples.push_back(RawSample{
            .point = point,
            .value = 0.0,
            .status = Quality::Timeout,
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

RawBatch PollSimulatorDriver::poll(
    const CollectionGroup& group,
    TimePoint deadline) {
    if (!started_) {
        throw std::logic_error("poll simulator is stopped");
    }
    if (group.device_id != device_.id) {
        throw std::invalid_argument("group does not belong to simulator device");
    }
    for (const auto& point : group.points) {
        if (!simulation_support::has_point(device_, point)) {
            throw std::invalid_argument("unknown point in group: " + point);
        }
    }

    const auto now = SchedulerClock::now();
    if (now >= deadline || latency_ > deadline - now) {
        return timeout_batch(group);
    }
    std::this_thread::sleep_for(latency_);

    ++sequence_;
    RawBatch batch{
        .device_id = group.device_id,
        .source = "simulator_poll",
        .samples = {},
    };
    const auto timestamp = unix_time_ns();
    for (const auto& point : group.points) {
        batch.samples.push_back(RawSample{
            .point = point,
            .value = simulation_support::simulated_value(point, sequence_),
            .status = Quality::Good,
            .source_time_ns = timestamp,
        });
    }
    return batch;
}

void register_poll_simulator_plugin(PluginRegistry& registry) {
    registry.register_driver(
        "simulator_poll",
        [](std::string_view settings_json)
            -> std::unique_ptr<IProtocolDriver> {
            constexpr std::string_view plugin{"poll simulator"};
            const auto settings =
                plugin_json::parse_object(settings_json, plugin);
            return std::make_unique<PollSimulatorDriver>(
                plugin_json::milliseconds_member(
                    settings, "latency_ms", plugin));
        });
}

}  // namespace gateway
