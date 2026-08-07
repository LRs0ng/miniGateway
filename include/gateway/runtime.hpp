#pragma once

#include "gateway/acquisition.hpp"
#include "gateway/bounded_queue.hpp"
#include "gateway/model.hpp"
#include "gateway/normalizer.hpp"
#include "gateway/processing.hpp"
#include "gateway/scheduler.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gateway {

struct DriverInstance {
    std::string device_id;
    std::unique_ptr<IProtocolDriver> driver;
};

using FinalEventHandler = std::function<void(const Event&)>;

struct RuntimeStats {
    QueueStats raw_queue;
    NormalizerStats normalizer;
    ProcessingStats processing;
    SchedulerStats scheduler;
    ExecutorStats executor;
    std::uint64_t delivered_events{0};
    std::uint64_t event_handler_errors{0};
};

class GatewayRuntime {
public:
    GatewayRuntime(
        GatewayConfig config,
        std::vector<DriverInstance> drivers,
        std::vector<std::unique_ptr<IDataProcessor>> processors,
        FinalEventHandler event_handler);
    ~GatewayRuntime();

    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] RuntimeStats stats() const;

private:
    void validate_and_bind();
    void process_batches();
    const DeviceConfig& find_device(const std::string& device_id) const;
    IProtocolDriver& find_driver(const std::string& device_id) const;

    GatewayConfig config_;
    std::vector<DriverInstance> drivers_;
    FinalEventHandler event_handler_;
    BoundedQueue<RawBatch> raw_queue_;
    Normalizer normalizer_;
    ProcessingPipeline pipeline_;
    std::unique_ptr<SequentialExecutor> executor_;
    std::unique_ptr<SchedulerEngine> scheduler_;
    std::jthread processing_worker_;

    mutable std::mutex lifecycle_mutex_;
    bool started_{false};
    bool stopped_{false};
    std::atomic<std::uint64_t> delivered_events_{0};
    std::atomic<std::uint64_t> event_handler_errors_{0};
};

}  // namespace gateway
