#include "gateway/bounded_queue.hpp"
#include "gateway/model.hpp"
#include "gateway/normalizer.hpp"
#include "gateway/processing.hpp"
#include "gateway/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;
using gateway::Event;
using gateway::Quality;
using gateway::Reading;
using gateway::Scalar;

class CheckFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail_check(
    const char* expression,
    const char* file,
    int line) {
    throw CheckFailure{
        std::string{file} + ":" + std::to_string(line) +
        ": check failed: " + expression};
}

#define CHECK(expression)                                                     \
    do {                                                                      \
        if (!(expression)) {                                                  \
            fail_check(#expression, __FILE__, __LINE__);                     \
        }                                                                     \
    } while (false)

void check_near(double actual, double expected, double tolerance = 1e-9) {
    CHECK(std::abs(actual - expected) <= tolerance);
}

Reading input_reading(std::string point, Scalar value) {
    return Reading{
        .point = std::move(point),
        .value = std::move(value),
        .quality = Quality::Good,
        .unit = {},
        .source_time_ns = 100,
        .received_time_ns = 200,
        .derived = false,
    };
}

Event event_with_value(std::string device_id, std::string point, Scalar value) {
    Event event;
    event.event_id = "test-event";
    event.device_id = std::move(device_id);
    event.source = "test";
    event.readings.push_back(input_reading(std::move(point), std::move(value)));
    return event;
}

void fixed_policy_uses_deadline_then_stable_order() {
    const auto due = gateway::TimePoint{} + 100ms;
    gateway::FixedIntervalSequentialPolicy policy{{
        gateway::ScheduleTask{
            .id = "later-order",
            .group_id = "group-b",
            .interval = 10ms,
            .next_due = due,
            .stable_order = 20,
        },
        gateway::ScheduleTask{
            .id = "earlier-order",
            .group_id = "group-a",
            .interval = 10ms,
            .next_due = due,
            .stable_order = 10,
        },
    }};

    const auto* selected = policy.select_due(due);
    CHECK(selected != nullptr);
    CHECK(selected->id == "earlier-order");

    gateway::FixedIntervalSequentialPolicy future_policy{{
        gateway::ScheduleTask{
            .id = "future",
            .group_id = "group",
            .interval = 10ms,
            .next_due = due + 1ms,
            .stable_order = 0,
        },
    }};
    CHECK(future_policy.select_due(due) == nullptr);
    CHECK(future_policy.next_wakeup() == due + 1ms);
}

void fixed_policy_skips_missed_cycles_without_drift() {
    const auto original_due = gateway::TimePoint{} + 100ms;
    gateway::FixedIntervalSequentialPolicy policy{{
        gateway::ScheduleTask{
            .id = "poll",
            .group_id = "group",
            .interval = 10ms,
            .next_due = original_due,
            .stable_order = 0,
        },
    }};

    auto* task = policy.select_due(original_due);
    CHECK(task != nullptr);
    policy.on_complete(*task, original_due + 35ms);

    CHECK(task->next_due == original_due + 40ms);
    CHECK(task->skipped_cycles == 3);
}

void bounded_queue_reports_backpressure_and_drains_after_close() {
    gateway::BoundedQueue<int> queue{2};

    CHECK(queue.try_push(1) == gateway::EnqueueResult::Accepted);
    CHECK(queue.try_push(2) == gateway::EnqueueResult::Accepted);
    CHECK(queue.try_push(3) == gateway::EnqueueResult::Full);

    queue.close();
    CHECK(queue.closed());
    CHECK(queue.try_push(4) == gateway::EnqueueResult::Stopping);

    int value = 0;
    CHECK(queue.wait_pop(value));
    CHECK(value == 1);
    CHECK(queue.wait_pop(value));
    CHECK(value == 2);
    CHECK(!queue.wait_pop(value));

    const auto stats = queue.stats();
    CHECK(stats.size == 0);
    CHECK(stats.capacity == 2);
    CHECK(stats.accepted == 2);
    CHECK(stats.rejected_full == 1);
}

gateway::PointConfig point(std::string name) {
    gateway::PointConfig config;
    config.name = std::move(name);
    config.type = gateway::ValueType::Double;
    return config;
}

void normalizer_converts_values_and_quality() {
    gateway::GatewayConfig config;
    gateway::DeviceConfig device;
    device.id = "device-1";

    auto scaled = point("scaled");
    scaled.unit = "C";
    scaled.scale = 2.0;
    scaled.offset = 1.0;
    device.points.push_back(std::move(scaled));
    device.points.push_back(point("timeout"));
    device.points.push_back(point("disconnected"));
    device.points.push_back(point("decode-error"));
    device.points.push_back(point("out-of-range"));
    device.points.push_back(point("bad"));
    config.devices.push_back(std::move(device));

    gateway::RawBatch batch;
    batch.device_id = "device-1";
    batch.source = "poll";
    batch.samples = {
        {.point = "scaled", .value = std::int64_t{3},
         .status = gateway::SampleStatus::Good},
        {.point = "timeout", .value = 1.0,
         .status = gateway::SampleStatus::Timeout},
        {.point = "disconnected", .value = 1.0,
         .status = gateway::SampleStatus::Disconnected},
        {.point = "decode-error", .value = 1.0,
         .status = gateway::SampleStatus::DecodeError},
        {.point = "out-of-range", .value = 1.0,
         .status = gateway::SampleStatus::OutOfRange},
        {.point = "bad", .value = 1.0,
         .status = gateway::SampleStatus::Bad},
        {.point = "unknown", .value = 1.0,
         .status = gateway::SampleStatus::Good},
    };

    gateway::Normalizer normalizer{config};
    const auto event = normalizer.normalize(std::move(batch));

    const auto* scaled_reading = gateway::find_reading(event, "scaled");
    CHECK(scaled_reading != nullptr);
    check_near(std::get<double>(scaled_reading->value), 7.0);
    CHECK(scaled_reading->unit == "C");
    CHECK(scaled_reading->quality == Quality::Good);

    CHECK(gateway::find_reading(event, "timeout")->quality == Quality::Timeout);
    CHECK(gateway::find_reading(event, "disconnected")->quality ==
          Quality::Disconnected);
    CHECK(gateway::find_reading(event, "decode-error")->quality ==
          Quality::DecodeError);
    CHECK(gateway::find_reading(event, "out-of-range")->quality ==
          Quality::OutOfRange);
    CHECK(gateway::find_reading(event, "bad")->quality == Quality::Bad);
    CHECK(gateway::find_reading(event, "unknown")->quality == Quality::Bad);

    const auto stats = normalizer.stats();
    CHECK(stats.batches == 1);
    CHECK(stats.readings == 7);
    CHECK(stats.bad_readings == 6);
}

void processors_append_derived_readings() {
    gateway::ProcessingContext context{.now_ns = 300};
    gateway::ThresholdProcessor threshold{"temperature", 10.0, "too-hot"};
    auto threshold_event = event_with_value("device-1", "temperature", 11.0);

    threshold.process(threshold_event, context);
    const auto* alarm = gateway::find_reading(threshold_event, "too-hot");
    CHECK(alarm != nullptr);
    CHECK(alarm->derived);
    CHECK(std::get<bool>(alarm->value));

    gateway::WindowAverageProcessor average{"temperature", 2, "temperature-avg"};
    auto first = event_with_value("device-1", "temperature", 2.0);
    average.process(first, context);
    CHECK(gateway::find_reading(first, "temperature-avg") == nullptr);

    auto second = event_with_value("device-1", "temperature", 4.0);
    average.process(second, context);
    const auto* average_reading = gateway::find_reading(second, "temperature-avg");
    CHECK(average_reading != nullptr);
    CHECK(average_reading->derived);
    check_near(std::get<double>(average_reading->value), 3.0);
}

class ThrowingProcessor final : public gateway::IDataProcessor {
public:
    std::string_view name() const noexcept override {
        return "throwing";
    }

    void process(Event&, gateway::ProcessingContext&) override {
        throw std::runtime_error{"expected processor failure"};
    }
};

class MarkerProcessor final : public gateway::IDataProcessor {
public:
    std::string_view name() const noexcept override {
        return "marker";
    }

    void process(Event& event, gateway::ProcessingContext&) override {
        auto reading = input_reading("after-error", true);
        reading.derived = true;
        event.readings.push_back(std::move(reading));
    }
};

void pipeline_isolates_processor_failures() {
    std::vector<std::unique_ptr<gateway::IDataProcessor>> processors;
    processors.push_back(std::make_unique<ThrowingProcessor>());
    processors.push_back(std::make_unique<MarkerProcessor>());
    gateway::ProcessingPipeline pipeline{std::move(processors)};
    Event event;

    pipeline.process(event);

    const auto* marker = gateway::find_reading(event, "after-error");
    CHECK(marker != nullptr);
    CHECK(marker->derived);
    const auto stats = pipeline.stats();
    CHECK(stats.events == 1);
    CHECK(stats.processor_errors == 1);
}

class RecordingExecutor final : public gateway::ICollectionExecutor {
public:
    void execute(const gateway::ScheduleTask&) override {
        std::size_t attempt = 0;
        {
            std::lock_guard lock(mutex_);
            attempt = ++attempts_;
            ++active_;
            max_active_ = std::max(max_active_, active_);
        }

        std::this_thread::sleep_for(1ms);

        {
            std::lock_guard lock(mutex_);
            --active_;
        }
        cv_.notify_all();

        if (attempt == 1) {
            throw std::runtime_error{"expected executor failure"};
        }
    }

    bool wait_for_attempts(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return cv_.wait_for(lock, timeout, [this, count] {
            return attempts_ >= count && active_ == 0;
        });
    }

    std::size_t attempts() const {
        std::lock_guard lock(mutex_);
        return attempts_;
    }

    std::size_t max_active() const {
        std::lock_guard lock(mutex_);
        return max_active_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t attempts_{0};
    std::size_t active_{0};
    std::size_t max_active_{0};
};

void scheduler_runs_serially_and_survives_executor_errors() {
    RecordingExecutor executor;
    const auto first_due = gateway::SchedulerClock::now();
    auto policy = std::make_unique<gateway::FixedIntervalSequentialPolicy>(
        std::vector<gateway::ScheduleTask>{
            gateway::ScheduleTask{
                .id = "poll-task",
                .group_id = "group",
                .interval = 5ms,
                .next_due = first_due,
                .stable_order = 0,
            },
        });
    gateway::SchedulerEngine scheduler{std::move(policy), executor};

    scheduler.start();
    const bool continued_after_error = executor.wait_for_attempts(3, 250ms);
    scheduler.stop();

    CHECK(continued_after_error);
    CHECK(executor.attempts() >= 3);
    CHECK(executor.max_active() == 1);

    const auto stats = scheduler.stats();
    CHECK(stats.executed >= 3);
    CHECK(stats.errors == 1);
    CHECK(stats.max_active_poll == 1);
}

struct TestCase {
    const char* name;
    void (*run)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"fixed policy ordering", fixed_policy_uses_deadline_then_stable_order},
        {"fixed policy overrun", fixed_policy_skips_missed_cycles_without_drift},
        {"bounded queue", bounded_queue_reports_backpressure_and_drains_after_close},
        {"normalizer", normalizer_converts_values_and_quality},
        {"derived processors", processors_append_derived_readings},
        {"pipeline isolation", pipeline_isolates_processor_failures},
        {"scheduler engine", scheduler_runs_serially_and_survives_executor_errors},
    };

    std::size_t failures = 0;
    for (const auto& test : tests) {
        try {
            test.run();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& error) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": " << error.what() << '\n';
        } catch (...) {
            ++failures;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << (tests.size() - failures) << '/' << tests.size()
              << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
