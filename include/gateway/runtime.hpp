#pragma once

#include "gateway/acquisition.hpp"
#include "gateway/bounded_queue.hpp"
#include "gateway/model.hpp"
#include "gateway/normalizer.hpp"
#include "gateway/event_publisher.hpp"
#include "gateway/processing.hpp"
#include "gateway/scheduler.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gateway {

struct RuntimeStats {
    QueueStats raw_queue;
    NormalizerStats normalizer;
    ProcessingStats processing;
    AcquisitionStats acquisition;
    SchedulerStats scheduler;
    EventPublisherStats event_publishers;
    std::uint64_t delivered_events{0};
};

class GatewayRuntime {
public:
    GatewayRuntime(
        GatewayConfig config,
        std::vector<DriverInstance> drivers,
        std::vector<std::unique_ptr<IDataProcessor>> processors,
        std::vector<EventPublisherInstance> event_publishers = {});
    ~GatewayRuntime();

    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] RuntimeStats stats() const;

private:
    void validate_and_configure();
    void poll_group(const ScheduleTask& task);
    void process_batches();
    void publish_event(const Event& event);
    const DeviceConfig& find_device(const std::string& device_id) const;
    const CollectionGroup& find_group(const std::string& group_id) const;
    IProtocolDriver& find_driver(const std::string& device_id) const;

    GatewayConfig config_;
    std::vector<DriverInstance> drivers_;
    std::vector<EventPublisherInstance> event_publishers_;
    BoundedQueue<RawBatch> raw_queue_;
    Normalizer normalizer_;
    ProcessingPipeline pipeline_;
    std::unique_ptr<SchedulerEngine> scheduler_;
    std::jthread processing_worker_;

    mutable std::mutex lifecycle_mutex_;
    bool started_{false};
    bool stopped_{false};
    std::atomic<std::uint64_t> polls_{0};
    std::atomic<std::uint64_t> poll_errors_{0};
    std::atomic<std::uint64_t> deadline_misses_{0};
    std::atomic<std::uint64_t> poll_queue_full_{0};
    std::atomic<std::uint64_t> delivered_events_{0};
    std::atomic<std::uint64_t> event_publish_attempts_{0};
    std::atomic<std::uint64_t> event_publish_accepted_{0};
    std::atomic<std::uint64_t> event_publish_unavailable_{0};
    std::atomic<std::uint64_t> event_publish_rejected_{0};
    std::atomic<std::uint64_t> event_publish_errors_{0};
};

}  // namespace gateway
