#pragma once

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gateway {

using SchedulerClock = std::chrono::steady_clock;
using TimePoint = SchedulerClock::time_point;

struct ScheduleTask {
    std::string group_id;
    std::chrono::milliseconds interval{};
    TimePoint next_due{};
    std::size_t stable_order{};
    std::uint64_t skipped_cycles{};
};

using PollCallback = std::function<void(const ScheduleTask&)>;

class ISchedulePolicy {
public:
    virtual ~ISchedulePolicy() = default;

    virtual ScheduleTask* select_due(TimePoint now) = 0;
    [[nodiscard]] virtual TimePoint next_wakeup() const = 0;
    virtual void on_complete(ScheduleTask& task, TimePoint finished_at) = 0;
    [[nodiscard]] virtual const std::vector<ScheduleTask>& tasks() const noexcept = 0;
};

class FixedIntervalSequentialPolicy final : public ISchedulePolicy {
public:
    explicit FixedIntervalSequentialPolicy(std::vector<ScheduleTask> tasks);

    ScheduleTask* select_due(TimePoint now) override;
    [[nodiscard]] TimePoint next_wakeup() const override;
    void on_complete(ScheduleTask& task, TimePoint finished_at) override;
    [[nodiscard]] const std::vector<ScheduleTask>& tasks() const noexcept override;

private:
    std::vector<ScheduleTask> tasks_;
};

// Timing fields are cumulative nanoseconds across all execution attempts.
struct SchedulerStats {
    std::uint64_t executed{};
    std::uint64_t errors{};
    std::uint64_t lateness_ns{};
    std::uint64_t duration_ns{};
    std::uint64_t skipped_cycles{};
};

class SchedulerEngine final {
public:
    SchedulerEngine(std::unique_ptr<ISchedulePolicy> policy,
                    PollCallback poll);
    ~SchedulerEngine();

    SchedulerEngine(const SchedulerEngine&) = delete;
    SchedulerEngine& operator=(const SchedulerEngine&) = delete;
    SchedulerEngine(SchedulerEngine&&) = delete;
    SchedulerEngine& operator=(SchedulerEngine&&) = delete;

    void start();
    void request_stop() noexcept;
    void join();
    void stop();

    [[nodiscard]] SchedulerStats stats() const;

private:
    void run();

    std::unique_ptr<ISchedulePolicy> policy_;
    PollCallback poll_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::jthread worker_;
    bool started_{false};
    bool stopping_{false};
    SchedulerStats stats_{};
};

}  // namespace gateway
