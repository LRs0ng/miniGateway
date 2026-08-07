#include "gateway/acquisition.hpp"

#include <utility>

namespace gateway {
namespace {

class ActivePollGuard {
public:
    explicit ActivePollGuard(std::atomic<std::uint64_t>& active)
        : active_(active) {}

    ~ActivePollGuard() {
        active_.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::atomic<std::uint64_t>& active_;
};

}  // namespace

SequentialExecutor::SequentialExecutor(SampleSink sink) : sink_(std::move(sink)) {
    if (!sink_) {
        throw std::invalid_argument("sample sink is required");
    }
}

void SequentialExecutor::bind(
    const CollectionGroup& group,
    IProtocolDriver& driver,
    CompiledPlan plan) {
    if (driver.capabilities().mode != AcquisitionMode::Poll) {
        throw std::invalid_argument("only poll drivers can be scheduled");
    }
    if (group.id.empty() || plan.group_id != group.id ||
        plan.device_id != group.device_id) {
        throw std::invalid_argument("compiled plan does not match collection group");
    }

    const auto [unused, inserted] = bindings_.emplace(
        group.id,
        Binding{.group = group, .driver = &driver, .plan = std::move(plan)});
    if (!inserted) {
        throw std::invalid_argument("duplicate collection group: " + group.id);
    }
}

void SequentialExecutor::execute(const ScheduleTask& task) {
    const auto binding = bindings_.find(task.group_id);
    if (binding == bindings_.end()) {
        throw std::out_of_range("collection group is not bound: " + task.group_id);
    }

    const auto active = active_poll_.fetch_add(1, std::memory_order_relaxed) + 1;
    ActivePollGuard active_guard{active_poll_};
    auto previous_max = max_active_poll_.load(std::memory_order_relaxed);
    while (active > previous_max &&
           !max_active_poll_.compare_exchange_weak(
               previous_max, active, std::memory_order_relaxed)) {
    }

    try {
        const auto started_at = SchedulerClock::now();
        const auto deadline = started_at + binding->second.group.timeout;
        auto batch = binding->second.driver->poll(binding->second.plan, deadline);
        if (SchedulerClock::now() > deadline) {
            deadline_misses_.fetch_add(1, std::memory_order_relaxed);
        }

        const auto result = sink_(std::move(batch));
        if (result == EnqueueResult::Full) {
            queue_full_.fetch_add(1, std::memory_order_relaxed);
        }
        polls_.fetch_add(1, std::memory_order_relaxed);
    } catch (...) {
        errors_.fetch_add(1, std::memory_order_relaxed);
        throw;
    }
}

ExecutorStats SequentialExecutor::stats() const {
    return ExecutorStats{
        .polls = polls_.load(std::memory_order_relaxed),
        .errors = errors_.load(std::memory_order_relaxed),
        .deadline_misses = deadline_misses_.load(std::memory_order_relaxed),
        .queue_full = queue_full_.load(std::memory_order_relaxed),
        .max_active_poll = max_active_poll_.load(std::memory_order_relaxed),
    };
}

}  // namespace gateway
