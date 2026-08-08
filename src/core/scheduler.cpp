#include "gateway/scheduler.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace gateway {
namespace {

std::uint64_t nonnegative_nanoseconds(SchedulerClock::duration duration) noexcept {
    const auto value =
        std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count();
    return value > 0 ? static_cast<std::uint64_t>(value) : 0;
}

void saturating_add(std::uint64_t& target, std::uint64_t value) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    target = value > maximum - target ? maximum : target + value;
}

}  // namespace

FixedIntervalSequentialPolicy::FixedIntervalSequentialPolicy(
    std::vector<ScheduleTask> tasks)
    : tasks_(std::move(tasks)) {
    std::unordered_set<std::string> group_ids;
    group_ids.reserve(tasks_.size());

    for (const auto& task : tasks_) {
        if (task.group_id.empty()) {
            throw std::invalid_argument("schedule group id must not be empty");
        }
        if (task.interval <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument("schedule task interval must be positive: " +
                                        task.group_id);
        }
        if (!group_ids.insert(task.group_id).second) {
            throw std::invalid_argument(
                "duplicate schedule group id: " + task.group_id);
        }
    }
}

ScheduleTask* FixedIntervalSequentialPolicy::select_due(TimePoint now) {
    ScheduleTask* selected = nullptr;

    for (auto& task : tasks_) {
        if (task.next_due > now) {
            continue;
        }
        if (selected == nullptr || task.next_due < selected->next_due ||
            (task.next_due == selected->next_due &&
             task.stable_order < selected->stable_order)) {
            selected = &task;
        }
    }

    return selected;
}

TimePoint FixedIntervalSequentialPolicy::next_wakeup() const {
    if (tasks_.empty()) {
        return TimePoint::max();
    }

    return std::min_element(
               tasks_.begin(), tasks_.end(),
               [](const ScheduleTask& lhs, const ScheduleTask& rhs) {
                   if (lhs.next_due != rhs.next_due) {
                       return lhs.next_due < rhs.next_due;
                   }
                   return lhs.stable_order < rhs.stable_order;
               })
        ->next_due;
}

void FixedIntervalSequentialPolicy::on_complete(ScheduleTask& task,
                                                 TimePoint finished_at) {
    task.next_due += task.interval;
    while (task.next_due <= finished_at) {
        task.next_due += task.interval;
        if (task.skipped_cycles < std::numeric_limits<std::uint64_t>::max()) {
            ++task.skipped_cycles;
        }
    }
}

const std::vector<ScheduleTask>& FixedIntervalSequentialPolicy::tasks() const
    noexcept {
    return tasks_;
}

SchedulerEngine::SchedulerEngine(std::unique_ptr<ISchedulePolicy> policy,
                                 PollCallback poll)
    : policy_(std::move(policy)), poll_(std::move(poll)) {
    if (policy_ == nullptr) {
        throw std::invalid_argument("schedule policy must not be null");
    }
    if (!poll_) {
        throw std::invalid_argument("poll callback must not be empty");
    }
}

SchedulerEngine::~SchedulerEngine() {
    stop();
}

void SchedulerEngine::start() {
    std::lock_guard lock(mutex_);
    if (started_) {
        throw std::logic_error("scheduler has already been started");
    }

    stopping_ = false;
    worker_ = std::jthread([this] { run(); });
    started_ = true;
}

void SchedulerEngine::request_stop() noexcept {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
}

void SchedulerEngine::join() {
    if (worker_.joinable()) {
        worker_.join();
    }
}

void SchedulerEngine::stop() {
    request_stop();
    join();
}

SchedulerStats SchedulerEngine::stats() const {
    std::lock_guard lock(mutex_);
    return stats_;
}

void SchedulerEngine::run() {
    std::unique_lock lock(mutex_);

    while (!stopping_) {
        const auto now = SchedulerClock::now();
        ScheduleTask* task = policy_->select_due(now);

        if (task == nullptr) {
            const auto wakeup = policy_->next_wakeup();
            if (wakeup == TimePoint::max()) {
                cv_.wait(lock, [this] { return stopping_; });
            } else {
                cv_.wait_until(lock, wakeup, [this] { return stopping_; });
            }
            continue;
        }

        const auto scheduled_at = task->next_due;
        const auto started_at = SchedulerClock::now();

        lock.unlock();
        bool failed = false;
        try {
            poll_(*task);
        } catch (...) {
            failed = true;
        }
        const auto finished_at = SchedulerClock::now();
        lock.lock();

        ++stats_.executed;
        if (failed) {
            ++stats_.errors;
        }
        saturating_add(stats_.lateness_ns,
                       nonnegative_nanoseconds(started_at - scheduled_at));
        saturating_add(stats_.duration_ns,
                       nonnegative_nanoseconds(finished_at - started_at));

        const auto skipped_before = task->skipped_cycles;
        policy_->on_complete(*task, finished_at);
        const auto skipped_after = task->skipped_cycles;
        saturating_add(stats_.skipped_cycles, skipped_after - skipped_before);
    }
}

}  // namespace gateway
