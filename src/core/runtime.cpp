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
    std::vector<EventPublisherInstance> event_publishers)
    : config_(std::move(config)),
      drivers_(std::move(drivers)),
      event_publishers_(std::move(event_publishers)),
      raw_queue_(config_.raw_queue_capacity),
      normalizer_(config_),
      pipeline_(std::move(processors)) {
    validate_and_configure();
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
    std::size_t started_publishers = 0;
    try {
        for (auto& instance : event_publishers_) {
            // Register the slot before calling user code so a partial start
            // can be rolled back if start() throws after acquiring resources.
            ++started_publishers;
            instance.publisher->start();
        }
        for (auto& instance : drivers_) {
            ++started_drivers;
            instance.driver->start();
        }

        const auto start_time = SchedulerClock::now();
        std::vector<ScheduleTask> tasks;
        tasks.reserve(config_.groups.size());
        for (std::size_t index = 0; index < config_.groups.size(); ++index) {
            const auto& group = config_.groups[index];
            tasks.push_back(ScheduleTask{
                .group_id = group.id,
                .interval = group.interval,
                .next_due = start_time + group.interval,
                .stable_order = index,
            });
        }

        auto policy = std::make_unique<FixedIntervalSequentialPolicy>(
            std::move(tasks));
        scheduler_ = std::make_unique<SchedulerEngine>(
            std::move(policy),
            [this](const ScheduleTask& task) { poll_group(task); });
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
        while (started_publishers > 0) {
            --started_publishers;
            event_publishers_[started_publishers].publisher->stop();
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
    for (auto instance = event_publishers_.rbegin();
         instance != event_publishers_.rend();
         ++instance) {
        instance->publisher->stop();
    }
}

RuntimeStats GatewayRuntime::stats() const {
    return RuntimeStats{
        .raw_queue = raw_queue_.stats(),
        .normalizer = normalizer_.stats(),
        .processing = pipeline_.stats(),
        .acquisition = AcquisitionStats{
            .polls = polls_.load(std::memory_order_relaxed),
            .errors = poll_errors_.load(std::memory_order_relaxed),
            .deadline_misses =
                deadline_misses_.load(std::memory_order_relaxed),
            .queue_full = poll_queue_full_.load(std::memory_order_relaxed),
        },
        .scheduler = scheduler_ ? scheduler_->stats() : SchedulerStats{},
        .event_publishers = EventPublisherStats{
            .attempts = event_publish_attempts_.load(std::memory_order_relaxed),
            .accepted = event_publish_accepted_.load(std::memory_order_relaxed),
            .unavailable =
                event_publish_unavailable_.load(std::memory_order_relaxed),
            .rejected = event_publish_rejected_.load(std::memory_order_relaxed),
            .errors = event_publish_errors_.load(std::memory_order_relaxed),
        },
        .delivered_events = delivered_events_.load(std::memory_order_relaxed),
    };
}

void GatewayRuntime::validate_and_configure() {
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
    }

    std::unordered_set<std::string> publisher_ids;
    for (auto& instance : event_publishers_) {
        if (instance.id.empty() || !instance.publisher) {
            throw std::invalid_argument(
                "every event publisher instance needs an id and publisher");
        }
        if (!publisher_ids.insert(instance.id).second) {
            throw std::invalid_argument(
                "duplicate event publisher: " + instance.id);
        }
        instance.publisher->configure(config_);
    }
}

void GatewayRuntime::poll_group(const ScheduleTask& task) {
    try {
        const auto& group = find_group(task.group_id);
        auto& driver = find_driver(group.device_id);
        const auto deadline = SchedulerClock::now() + group.timeout;
        auto batch = driver.poll(group, deadline);
        if (SchedulerClock::now() > deadline) {
            deadline_misses_.fetch_add(1, std::memory_order_relaxed);
        }

        const auto result = raw_queue_.try_push(std::move(batch));
        if (result == EnqueueResult::Full) {
            poll_queue_full_.fetch_add(1, std::memory_order_relaxed);
        }
        polls_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        poll_errors_.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
}

void GatewayRuntime::process_batches() {
    RawBatch batch;
    while (raw_queue_.wait_pop(batch)) {
        try {
            auto event = normalizer_.normalize(std::move(batch));
            pipeline_.process(event);
            delivered_events_.fetch_add(1, std::memory_order_relaxed);
            publish_event(event);
        } catch (...) {
            // A malformed batch must not terminate the processing worker.
        }
    }
}

void GatewayRuntime::publish_event(const Event& event) {
    for (auto& instance : event_publishers_) {
        event_publish_attempts_.fetch_add(1, std::memory_order_relaxed);
        try {
            switch (instance.publisher->publish(event)) {
                case EventPublishResult::Accepted:
                    event_publish_accepted_.fetch_add(
                        1, std::memory_order_relaxed);
                    break;
                case EventPublishResult::Unavailable:
                    event_publish_unavailable_.fetch_add(
                        1, std::memory_order_relaxed);
                    break;
                case EventPublishResult::Rejected:
                    event_publish_rejected_.fetch_add(
                        1, std::memory_order_relaxed);
                    break;
            }
        } catch (...) {
            event_publish_errors_.fetch_add(1, std::memory_order_relaxed);
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

const CollectionGroup& GatewayRuntime::find_group(
    const std::string& group_id) const {
    const auto group = std::find_if(
        config_.groups.begin(), config_.groups.end(),
        [&group_id](const CollectionGroup& item) { return item.id == group_id; });
    if (group == config_.groups.end()) {
        throw std::out_of_range("scheduled collection group was not found: " + group_id);
    }
    return *group;
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
