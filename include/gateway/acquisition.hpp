#pragma once

#include "gateway/bounded_queue.hpp"
#include "gateway/model.hpp"
#include "gateway/scheduler.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace gateway {

enum class AcquisitionMode {
    Poll,
    Push,
};

struct DriverCapabilities {
    AcquisitionMode mode{AcquisitionMode::Poll};
    bool supports_write{false};
};

struct CompiledPlan {
    std::string group_id;
    std::string device_id;
    std::vector<std::string> points;
};

using SampleSink = std::function<EnqueueResult(RawBatch&&)>;

class IProtocolDriver {
public:
    virtual ~IProtocolDriver() = default;

    virtual DriverCapabilities capabilities() const = 0;
    virtual void configure(const DeviceConfig& device, SampleSink sink) = 0;
    virtual void start() = 0;
    virtual void stop() noexcept = 0;

    virtual CompiledPlan compile(const CollectionGroup&) {
        throw std::logic_error("driver does not support polling");
    }

    virtual RawBatch poll(const CompiledPlan&, TimePoint) {
        throw std::logic_error("driver does not support polling");
    }
};

struct ExecutorStats {
    std::uint64_t polls{0};
    std::uint64_t errors{0};
    std::uint64_t deadline_misses{0};
    std::uint64_t queue_full{0};
    std::uint64_t max_active_poll{0};
};

class SequentialExecutor final : public ICollectionExecutor {
public:
    explicit SequentialExecutor(SampleSink sink);

    void bind(
        const CollectionGroup& group,
        IProtocolDriver& driver,
        CompiledPlan plan);
    void execute(const ScheduleTask& task) override;

    [[nodiscard]] ExecutorStats stats() const;

private:
    struct Binding {
        CollectionGroup group;
        IProtocolDriver* driver{nullptr};
        CompiledPlan plan;
    };

    SampleSink sink_;
    std::unordered_map<std::string, Binding> bindings_;
    std::atomic<std::uint64_t> polls_{0};
    std::atomic<std::uint64_t> errors_{0};
    std::atomic<std::uint64_t> deadline_misses_{0};
    std::atomic<std::uint64_t> queue_full_{0};
    std::atomic<std::uint64_t> active_poll_{0};
    std::atomic<std::uint64_t> max_active_poll_{0};
};

}  // namespace gateway
