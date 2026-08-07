#include "gateway/runtime.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gateway {

GatewayRuntime::GatewayRuntime(
    GatewayConfig config,
    std::vector<DriverInstance> drivers,
    std::vector<std::unique_ptr<IDataProcessor>> processors,
    FinalEventHandler event_handler)
    : config_(std::move(config)),
      drivers_(std::move(drivers)),
      event_handler_(std::move(event_handler)),
      raw_queue_(config_.raw_queue_capacity),
      normalizer_(config_),
      pipeline_(std::move(processors)) {
    if (!event_handler_) {
        throw std::invalid_argument("final event handler is required");
    }

    auto sink = [this](RawBatch&& batch) {
        return raw_queue_.try_push(std::move(batch));
    };
    executor_ = std::make_unique<SequentialExecutor>(sink);
    validate_and_bind();
}

GatewayRuntime::~GatewayRuntime() {
    stop();
}

void GatewayRuntime::start() {
    std::lock_guard lock(lifecycle_mutex_);
    if (started_) {
        throw std::logic_error("gateway runtime can only be started once");
    }
    started_ = true;

    processing_worker_ = std::jthread([this] { process_batches(); });
    std::size_t started_drivers = 0;
    try {
        for (auto& instance : drivers_) {
            instance.driver->start();
            ++started_drivers;
        }

        const auto start_time = SchedulerClock::now();
        std::vector<ScheduleTask> tasks;
        tasks.reserve(config_.groups.size());
        for (std::size_t index = 0; index < config_.groups.size(); ++index) {
            const auto& group = config_.groups[index];
            tasks.push_back(ScheduleTask{
                .id = group.id,
                .group_id = group.id,
                .interval = group.interval,
                .next_due = start_time + group.interval,
                .stable_order = index,
            });
        }

        auto policy = std::make_unique<FixedIntervalSequentialPolicy>(
            std::move(tasks));
        scheduler_ = std::make_unique<SchedulerEngine>(
            std::move(policy), *executor_);
        scheduler_->start();
    } catch (...) {
        if (scheduler_) {
            scheduler_->stop();
        }
        while (started_drivers > 0) {
            --started_drivers;
            drivers_[started_drivers].driver->stop();
        }
        raw_queue_.close();
        if (processing_worker_.joinable()) {
            processing_worker_.join();
        }
        stopped_ = true;
        throw;
    }
}

void GatewayRuntime::stop() noexcept {
    std::unique_lock lock(lifecycle_mutex_);
    if (!started_ || stopped_) {
        return;
    }
    stopped_ = true;

    if (scheduler_) {
        scheduler_->request_stop();
    }
    for (auto& instance : drivers_) {
        if (instance.driver->capabilities().mode == AcquisitionMode::Push) {
            instance.driver->stop();
        }
    }
    if (scheduler_) {
        scheduler_->join();
    }
    for (auto& instance : drivers_) {
        if (instance.driver->capabilities().mode == AcquisitionMode::Poll) {
            instance.driver->stop();
        }
    }

    raw_queue_.close();
    lock.unlock();
    if (processing_worker_.joinable()) {
        processing_worker_.join();
    }
}

RuntimeStats GatewayRuntime::stats() const {
    return RuntimeStats{
        .raw_queue = raw_queue_.stats(),
        .normalizer = normalizer_.stats(),
        .processing = pipeline_.stats(),
        .scheduler = scheduler_ ? scheduler_->stats() : SchedulerStats{},
        .executor = executor_->stats(),
        .delivered_events = delivered_events_.load(std::memory_order_relaxed),
        .event_handler_errors =
            event_handler_errors_.load(std::memory_order_relaxed),
    };
}

void GatewayRuntime::validate_and_bind() {
    std::unordered_set<std::string> driver_devices;
    for (auto& instance : drivers_) {
        if (instance.device_id.empty() || !instance.driver) {
            throw std::invalid_argument("every driver instance needs a device id and driver");
        }
        if (!driver_devices.insert(instance.device_id).second) {
            throw std::invalid_argument(
                "duplicate driver for device: " + instance.device_id);
        }
        const auto& device = find_device(instance.device_id);
        instance.driver->configure(
            device,
            [this](RawBatch&& batch) {
                return raw_queue_.try_push(std::move(batch));
            });
    }

    if (driver_devices.size() != config_.devices.size()) {
        throw std::invalid_argument("each configured device must have one driver instance");
    }

    std::unordered_set<std::string> group_ids;
    for (const auto& group : config_.groups) {
        if (group.id.empty() || !group_ids.insert(group.id).second) {
            throw std::invalid_argument("collection group ids must be non-empty and unique");
        }
        if (group.interval <= std::chrono::milliseconds::zero() ||
            group.timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "collection group interval and timeout must be positive");
        }
        auto& driver = find_driver(group.device_id);
        if (driver.capabilities().mode != AcquisitionMode::Poll) {
            throw std::invalid_argument(
                "push device cannot own a collection group: " + group.id);
        }
        executor_->bind(group, driver, driver.compile(group));
    }
}

void GatewayRuntime::process_batches() {
    RawBatch batch;
    while (raw_queue_.wait_pop(batch)) {
        try {
            auto event = normalizer_.normalize(std::move(batch));
            pipeline_.process(event);
            try {
                event_handler_(event);
                delivered_events_.fetch_add(1, std::memory_order_relaxed);
            } catch (...) {
                event_handler_errors_.fetch_add(1, std::memory_order_relaxed);
            }
        } catch (...) {
            event_handler_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

const DeviceConfig& GatewayRuntime::find_device(const std::string& device_id) const {
    const auto device = std::find_if(
        config_.devices.begin(), config_.devices.end(),
        [&device_id](const DeviceConfig& item) { return item.id == device_id; });
    if (device == config_.devices.end()) {
        throw std::invalid_argument("driver references unknown device: " + device_id);
    }
    return *device;
}

IProtocolDriver& GatewayRuntime::find_driver(const std::string& device_id) const {
    const auto instance = std::find_if(
        drivers_.begin(), drivers_.end(),
        [&device_id](const DriverInstance& item) {
            return item.device_id == device_id;
        });
    if (instance == drivers_.end()) {
        throw std::invalid_argument("group references device without driver: " + device_id);
    }
    return *instance->driver;
}

}  // namespace gateway
