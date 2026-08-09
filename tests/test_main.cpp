#include "gateway/bounded_queue.hpp"
#include "gateway/config.hpp"
#include "gateway/model.hpp"
#include "gateway/normalizer.hpp"
#include "gateway/plugin_registry.hpp"
#include "gateway/processing.hpp"
#include "gateway/runtime.hpp"
#include "gateway/scheduler.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef GATEWAY_TEST_CONFIG_PATH
#define GATEWAY_TEST_CONFIG_PATH "example/config.json"
#endif

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

gateway::PointConfig point(std::string name) {
    gateway::PointConfig config;
    config.name = std::move(name);
    config.type = gateway::ValueType::Double;
    return config;
}

void fixed_policy_uses_deadline_then_stable_order() {
    const auto due = gateway::TimePoint{} + 100ms;
    gateway::FixedIntervalSequentialPolicy policy{{
        gateway::ScheduleTask{
            .group_id = "group-b",
            .interval = 10ms,
            .next_due = due,
            .stable_order = 20,
        },
        gateway::ScheduleTask{
            .group_id = "group-a",
            .interval = 10ms,
            .next_due = due,
            .stable_order = 10,
        },
    }};

    const auto* selected = policy.select_due(due);
    CHECK(selected != nullptr);
    CHECK(selected->group_id == "group-a");

    gateway::FixedIntervalSequentialPolicy future_policy{{
        gateway::ScheduleTask{
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
         .status = gateway::Quality::Good},
        {.point = "timeout", .value = 1.0,
         .status = gateway::Quality::Timeout},
        {.point = "disconnected", .value = 1.0,
         .status = gateway::Quality::Disconnected},
        {.point = "decode-error", .value = 1.0,
         .status = gateway::Quality::DecodeError},
        {.point = "out-of-range", .value = 1.0,
         .status = gateway::Quality::OutOfRange},
        {.point = "bad", .value = 1.0,
         .status = gateway::Quality::Bad},
        {.point = "unknown", .value = 1.0,
         .status = gateway::Quality::Good},
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

class ThrowingProcessor final : public gateway::IDataProcessor {
public:
    void process(Event&, gateway::ProcessingContext&) override {
        throw std::runtime_error{"expected processor failure"};
    }
};

class MarkerProcessor final : public gateway::IDataProcessor {
public:
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

class StubPollDriver final : public gateway::IProtocolDriver {
public:
    gateway::DriverCapabilities capabilities() const override {
        return {.mode = gateway::AcquisitionMode::Poll};
    }
    void configure(const gateway::DeviceConfig&, gateway::SampleSink) override {}
    void start() override {}
    void stop() noexcept override {}
};

class StubProcessor final : public gateway::IDataProcessor {
public:
    void process(Event&, gateway::ProcessingContext&) override {}
};

class StubPublisher final : public gateway::IEventPublisher {
public:
    void configure(const gateway::GatewayConfig&) override {}
    void start() override {}
    gateway::EventPublishResult publish(const Event&) override {
        return gateway::EventPublishResult::Accepted;
    }
    void stop() noexcept override {}
};

void registry_creates_only_enabled_generic_plugins() {
    gateway::PluginRegistry registry;
    registry.register_driver(
        "stub-driver",
        [](std::string_view) -> std::unique_ptr<gateway::IProtocolDriver> {
            return std::make_unique<StubPollDriver>();
        });
    registry.register_processor(
        "stub-processor",
        [](std::string_view) -> std::unique_ptr<gateway::IDataProcessor> {
            return std::make_unique<StubProcessor>();
        });
    registry.register_event_publisher(
        "stub-publisher",
        [](std::string_view) -> std::unique_ptr<gateway::IEventPublisher> {
            return std::make_unique<StubPublisher>();
        });

    gateway::ApplicationConfig config;
    config.gateway.devices.push_back(gateway::DeviceConfig{
        .id = "device",
        .driver = {
            .type = "stub-driver",
            .settings_json = "{}",
            .library = {},
        },
        .connection = {},
        .points = {},
    });
    config.processors.push_back({
        "processor", "stub-processor", true, "{}", {}});
    config.processors.push_back({
        "disabled", "unknown", false, "{}", {}});
    config.event_publishers.push_back(
        {"publisher", "stub-publisher", true, "{}", {}});
    config.event_publishers.push_back(
        {"disabled", "unknown", false, "{}", {}});

    auto instances = registry.create(config);
    CHECK(instances.drivers.size() == 1);
    CHECK(instances.processors.size() == 1);
    CHECK(instances.event_publishers.size() == 1);
    CHECK(instances.drivers.front().driver->capabilities().mode ==
          gateway::AcquisitionMode::Poll);
    bool duplicate_rejected = false;
    try {
        registry.register_driver(
            "stub-driver",
            [](std::string_view) -> std::unique_ptr<gateway::IProtocolDriver> {
                return std::make_unique<StubPollDriver>();
            });
    } catch (const std::invalid_argument&) {
        duplicate_rejected = true;
    }
    CHECK(duplicate_rejected);

    gateway::ApplicationConfig unknown;
    unknown.processors.push_back({
        "missing", "not-registered", true, "{}", {}});
    bool unknown_rejected = false;
    try {
        static_cast<void>(registry.create(unknown));
    } catch (const std::invalid_argument& error) {
        unknown_rejected = std::string{error.what()}.find(
            "unknown processor plugin type") != std::string::npos;
    }
    CHECK(unknown_rejected);
}

class OneShotPushDriver final : public gateway::IProtocolDriver {
public:
    gateway::DriverCapabilities capabilities() const override {
        return {.mode = gateway::AcquisitionMode::Push};
    }

    void configure(
        const gateway::DeviceConfig& device,
        gateway::SampleSink sink) override {
        device_ = device;
        sink_ = std::move(sink);
        configured_ = true;
    }

    void start() override {
        CHECK(configured_);
        started_ = true;
        gateway::RawBatch batch{
            .device_id = device_.id,
            .source = "one-shot",
            .samples = {{
                .point = device_.points.front().name,
                .value = 12.0,
                .status = gateway::Quality::Good,
                .source_time_ns = gateway::unix_time_ns(),
            }},
        };
        CHECK(sink_(std::move(batch)) == gateway::EnqueueResult::Accepted);
    }

    void stop() noexcept override {
        stopped_ = true;
    }

    gateway::DeviceConfig device_;
    gateway::SampleSink sink_;
    bool configured_{false};
    bool started_{false};
    bool stopped_{false};
};

class RecordingEventPublisher final : public gateway::IEventPublisher {
public:
    void configure(const gateway::GatewayConfig&) override {
        configured_ = true;
    }

    void start() override {
        CHECK(configured_);
        started_ = true;
    }

    gateway::EventPublishResult publish(const Event& event) override {
        CHECK(started_);
        ++published_;
        last_device_id_ = event.device_id;
        return gateway::EventPublishResult::Accepted;
    }

    void stop() noexcept override {
        stopped_ = true;
    }

    bool configured_{false};
    bool started_{false};
    bool stopped_{false};
    std::uint64_t published_{0};
    std::string last_device_id_;
};

void runtime_dispatches_without_a_builtin_event_handler() {
    gateway::GatewayConfig config;
    config.raw_queue_capacity = 8;
    config.devices.push_back(gateway::DeviceConfig{
        .id = "push-device",
        .driver = {.type = "test", .library = {}},
        .connection = {},
        .points = {point("value")},
    });

    auto driver = std::make_unique<OneShotPushDriver>();
    auto* driver_observer = driver.get();
    std::vector<gateway::DriverInstance> drivers;
    drivers.push_back({"push-device", std::move(driver)});

    auto publisher = std::make_unique<RecordingEventPublisher>();
    auto* publisher_observer = publisher.get();
    std::vector<gateway::EventPublisherInstance> publishers;
    publishers.push_back({"recording", std::move(publisher)});

    gateway::GatewayRuntime runtime{
        std::move(config), std::move(drivers), {}, std::move(publishers)};
    runtime.start();
    std::this_thread::sleep_for(20ms);
    runtime.stop();

    const auto stats = runtime.stats();
    CHECK(driver_observer->configured_);
    CHECK(driver_observer->started_);
    CHECK(driver_observer->stopped_);
    CHECK(publisher_observer->configured_);
    CHECK(publisher_observer->started_);
    CHECK(publisher_observer->stopped_);
    CHECK(publisher_observer->published_ == 1);
    CHECK(publisher_observer->last_device_id_ == "push-device");
    CHECK(stats.delivered_events == 1);
    CHECK(stats.event_publishers.attempts == 1);
    CHECK(stats.event_publishers.accepted == 1);
}

void runtime_rejects_poll_groups_for_push_drivers() {
    gateway::GatewayConfig config;
    config.devices.push_back(gateway::DeviceConfig{
        .id = "push-device",
        .driver = {.type = "test", .library = {}},
        .connection = {},
        .points = {point("value")},
    });
    config.groups.push_back(gateway::CollectionGroup{
        .id = "invalid",
        .device_id = "push-device",
        .interval = 10ms,
        .timeout = 5ms,
        .points = {"value"},
    });
    std::vector<gateway::DriverInstance> drivers;
    drivers.push_back({"push-device", std::make_unique<OneShotPushDriver>()});

    bool rejected = false;
    try {
        gateway::GatewayRuntime runtime{
            std::move(config), std::move(drivers), {}, {}};
    } catch (const std::invalid_argument& error) {
        rejected = std::string{error.what()}.find("push device") !=
            std::string::npos;
    }
    CHECK(rejected);
}

class RecordingPoller final {
public:
    void operator()(const gateway::ScheduleTask&) {
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

void scheduler_runs_serially_and_survives_poll_errors() {
    RecordingPoller poller;
    const auto first_due = gateway::SchedulerClock::now();
    auto policy = std::make_unique<gateway::FixedIntervalSequentialPolicy>(
        std::vector<gateway::ScheduleTask>{gateway::ScheduleTask{
            .group_id = "group",
            .interval = 5ms,
            .next_due = first_due,
            .stable_order = 0,
        }});
    gateway::SchedulerEngine scheduler{
        std::move(policy),
        [&poller](const gateway::ScheduleTask& task) { poller(task); }};

    scheduler.start();
    const bool continued_after_error = poller.wait_for_attempts(3, 250ms);
    scheduler.stop();

    CHECK(continued_after_error);
    CHECK(poller.attempts() >= 3);
    CHECK(poller.max_active() == 1);
    const auto stats = scheduler.stats();
    CHECK(stats.executed >= 3);
    CHECK(stats.errors == 1);
}

constexpr std::string_view valid_config = R"json(
{
  "runtime": {"run_duration_ms": 25, "raw_queue_capacity": 4},
  "devices": [{
    "id": "machine", "driver": "unrecognized_driver",
    "library": "./unrecognized_driver.dll",
    "driver_config": {"vendor_option": 2},
    "connection": {"endpoint": "loopback"},
    "points": [{"name": "temperature", "type": "double", "unit": "C",
      "address": {"register": "1"}, "scale": 2.0, "offset": 1.0,
      "minimum": -40.0, "maximum": 120.0}]
  }],
  "groups": [{"id": "temperature-group", "device_id": "machine",
    "interval_ms": 10, "timeout_ms": 5, "points": ["temperature"]}],
  "processors": [{"id": "custom", "type": "unrecognized_processor",
    "enabled": true, "config": {"vendor_option": "kept"}}],
  "event_publishers": [{"id": "custom-output",
    "type": "unrecognized_publisher", "enabled": false,
    "config": {"endpoint": "kept"}}]
}
)json";

void config_parser_preserves_generic_plugin_specs() {
    const auto config = gateway::parse_config(valid_config, "config-test");
    CHECK(config.run_duration == 25ms);
    CHECK(config.gateway.raw_queue_capacity == 4);
    CHECK(config.gateway.devices.size() == 1);
    CHECK(config.gateway.devices.front().points.front().minimum.has_value());
    CHECK(config.gateway.groups.size() == 1);
    CHECK(config.gateway.devices.front().driver.type == "unrecognized_driver");
    CHECK(config.gateway.devices.front().driver.library ==
          std::filesystem::path{"./unrecognized_driver.dll"});
    CHECK(config.gateway.devices.front().driver.settings_json.find(
              "vendor_option") != std::string::npos);
    CHECK(config.processors.size() == 1);
    CHECK(config.processors.front().type == "unrecognized_processor");
    CHECK(config.processors.front().settings_json.find("kept") !=
          std::string::npos);
    CHECK(config.event_publishers.size() == 1);
    CHECK(!config.event_publishers.front().enabled);
}

void config_parser_rejects_invalid_cross_references() {
    std::string text{valid_config};
    const auto point_name = text.find("[\"temperature\"]");
    CHECK(point_name != std::string::npos);
    text.replace(point_name, std::string{"[\"temperature\"]"}.size(),
                 "[\"missing\"]");

    bool rejected = false;
    try {
        static_cast<void>(gateway::parse_config(text, "invalid-test"));
    } catch (const std::runtime_error& error) {
        rejected = std::string{error.what()}.find("unknown point") !=
            std::string::npos;
    }
    CHECK(rejected);
}

void config_loader_reads_repository_config() {
    const auto config = gateway::load_config(GATEWAY_TEST_CONFIG_PATH);
    CHECK(config.run_duration == 2200ms);
    CHECK(config.gateway.raw_queue_capacity == 32);
    CHECK(config.gateway.devices.size() == 2);
    CHECK(config.gateway.groups.size() == 2);
    CHECK(config.gateway.devices.front().driver.type == "simulator_poll");
    CHECK(config.gateway.devices.back().driver.type == "simulator_push");
    CHECK(config.processors.size() == 3);
    CHECK(config.event_publishers.size() == 2);
    CHECK(config.event_publishers.front().type == "print");
    CHECK(!config.event_publishers.back().enabled);
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
        {"pipeline isolation", pipeline_isolates_processor_failures},
        {"generic plugin registry", registry_creates_only_enabled_generic_plugins},
        {"runtime publisher dispatch", runtime_dispatches_without_a_builtin_event_handler},
        {"runtime capability validation", runtime_rejects_poll_groups_for_push_drivers},
        {"scheduler engine", scheduler_runs_serially_and_survives_poll_errors},
        {"generic config parser", config_parser_preserves_generic_plugin_specs},
        {"config reference validation", config_parser_rejects_invalid_cross_references},
        {"config file", config_loader_reads_repository_config},
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
