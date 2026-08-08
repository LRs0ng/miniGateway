#pragma once

#include "gateway/acquisition.hpp"
#include "gateway/plugin_registry.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace gateway {

class PollSimulatorDriver final : public IProtocolDriver {
public:
    explicit PollSimulatorDriver(
        std::chrono::milliseconds latency = std::chrono::milliseconds{20});

    [[nodiscard]] DriverCapabilities capabilities() const override;
    void configure(const DeviceConfig& device, SampleSink sink) override;
    void start() override;
    void stop() noexcept override;
    [[nodiscard]] RawBatch poll(
        const CollectionGroup& group,
        TimePoint deadline) override;

private:
    DeviceConfig device_;
    std::chrono::milliseconds latency_;
    std::uint64_t sequence_{0};
    bool configured_{false};
    bool started_{false};
};

struct PushSimulatorStats {
    std::uint64_t produced{0};
    std::uint64_t queue_full{0};
};

class PushSimulatorDriver final : public IProtocolDriver {
public:
    explicit PushSimulatorDriver(std::chrono::milliseconds interval);
    ~PushSimulatorDriver() override;

    [[nodiscard]] DriverCapabilities capabilities() const override;
    void configure(const DeviceConfig& device, SampleSink sink) override;
    void start() override;
    void stop() noexcept override;

    [[nodiscard]] PushSimulatorStats stats() const;

private:
    void run(std::stop_token stop_token);
    [[nodiscard]] RawBatch make_batch();

    DeviceConfig device_;
    SampleSink sink_;
    std::chrono::milliseconds interval_;
    std::jthread worker_;
    mutable std::mutex mutex_;
    std::condition_variable wakeup_;
    std::uint64_t sequence_{0};
    bool configured_{false};
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::uint64_t> queue_full_{0};
};

void register_poll_simulator_plugin(PluginRegistry& registry);
void register_push_simulator_plugin(PluginRegistry& registry);

}  // namespace gateway
