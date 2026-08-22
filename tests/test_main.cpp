#include "gateway/bounded_queue.hpp"
#include "gateway/config.hpp"
#include "gateway/control.hpp"
#include "gateway/model.hpp"
#include "gateway/normalizer.hpp"
#include "gateway/plugin_registry.hpp"
#include "gateway/processing.hpp"
#include "gateway/runtime.hpp"
#include "gateway/scheduler.hpp"

#include "control_internal.hpp"

#include <algorithm>
#include <atomic>
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
#if defined(_WIN32)
#define GATEWAY_TEST_CONFIG_PATH \
    "example/configs/defconfig_windows.json"
#else
#define GATEWAY_TEST_CONFIG_PATH \
    "example/configs/defconfig_linux.json"
#endif
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

void normalizer_preserves_byte_arrays() {
    gateway::GatewayConfig config;
    gateway::DeviceConfig device;
    device.id = "camera-1";
    auto snapshot = point("snapshot");
    snapshot.type = gateway::ValueType::ByteArray;
    device.points.push_back(std::move(snapshot));
    config.devices.push_back(std::move(device));

    gateway::RawBatch batch{
        .device_id = "camera-1",
        .source = "camera",
        .samples = {{
            .point = "snapshot",
            .value = gateway::ByteArray{0x00U, 0x7fU, 0xffU},
            .status = gateway::Quality::Good,
            .source_time_ns = 123,
        }},
    };

    gateway::Normalizer normalizer{config};
    const auto event = normalizer.normalize(std::move(batch));
    const auto* reading = gateway::find_reading(event, "snapshot");
    CHECK(reading != nullptr);
    CHECK(reading->quality == Quality::Good);
    CHECK(std::get<gateway::ByteArray>(reading->value) ==
          gateway::ByteArray({0x00U, 0x7fU, 0xffU}));
    CHECK(!gateway::numeric_value(reading->value).has_value());
    CHECK(gateway::scalar_to_string(reading->value) == "<3 bytes>");

    gateway::RawBatch invalid_batch{
        .device_id = "camera-1",
        .source = "camera",
        .samples = {{
            .point = "snapshot",
            .value = std::string{"not binary"},
            .status = gateway::Quality::Good,
            .source_time_ns = 124,
        }},
    };
    const auto invalid_event = normalizer.normalize(std::move(invalid_batch));
    const auto* invalid_reading =
        gateway::find_reading(invalid_event, "snapshot");
    CHECK(invalid_reading != nullptr);
    CHECK(invalid_reading->quality == Quality::DecodeError);
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

class ControlSubmittingProcessor final : public gateway::IDataProcessor {
public:
    void process(Event& event, gateway::ProcessingContext& context) override {
        result_ = context.submit_control(gateway::DeviceControlRequest{
            .request_id = "processor-request",
            .device_id = event.device_id,
            .command = "set",
            .arguments = {{"enabled", true}},
        });
    }

    gateway::ControlSubmitResult result_{gateway::ControlSubmitResult::Stopping};
};

gateway::DeviceControlRequest control_request(
    std::string request_id,
    std::string device_id = "device",
    std::string command = "set") {
    return gateway::DeviceControlRequest{
        .request_id = std::move(request_id),
        .device_id = std::move(device_id),
        .command = std::move(command),
        .arguments = {},
    };
}

class ControlCompletionProbe final {
public:
    gateway::ControlCompletion callback() {
        return [this](gateway::DeviceControlResult&& result) {
            {
                std::lock_guard lock(mutex_);
                results_.push_back(std::move(result));
            }
            condition_.notify_all();
        };
    }

    bool wait_for(
        std::size_t count,
        std::chrono::milliseconds timeout = 500ms) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, count] {
            return results_.size() >= count;
        });
    }

    std::size_t size() const {
        std::lock_guard lock(mutex_);
        return results_.size();
    }

    gateway::DeviceControlResult result(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return results_.at(index);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::vector<gateway::DeviceControlResult> results_;
};

class ConcurrentStopProbe final {
public:
    void entered() {
        {
            std::lock_guard lock(mutex_);
            ++entered_;
        }
        condition_.notify_all();
    }

    void returned() {
        {
            std::lock_guard lock(mutex_);
            ++returned_;
        }
        condition_.notify_all();
    }

    bool wait_for_entered(
        std::size_t count,
        std::chrono::milliseconds timeout = 500ms) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this, count] {
            return entered_ >= count;
        });
    }

    std::size_t returned_count() const {
        std::lock_guard lock(mutex_);
        return returned_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::size_t entered_{0};
    std::size_t returned_{0};
};

class ControlProbeDriver final : public gateway::IProtocolDriver {
public:
    explicit ControlProbeDriver(bool block_controls = false)
        : block_controls_(block_controls) {}

    gateway::DriverCapabilities capabilities() const override {
        return {.mode = gateway::AcquisitionMode::Poll};
    }

    void configure(const gateway::DeviceConfig&, gateway::SampleSink) override {}
    void start() override {}
    void stop() noexcept override {
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
        }
        entered_.notify_all();
    }

    gateway::DeviceControlResult control(
        const gateway::DeviceControlRequest& request) override {
        std::unique_lock lock(mutex_);
        ++calls_;
        request_ids_.push_back(request.request_id);
        entered_.notify_all();
        if (block_controls_) {
            static_cast<void>(entered_.wait_for(
                lock, 5s, [this] { return released_; }));
        }
        return gateway::DeviceControlResult{
            .request_id = request.request_id,
            .status = gateway::DeviceControlStatus::Succeeded,
            .outputs = {},
            .message = {},
        };
    }

    bool wait_for_calls(
        std::size_t count,
        std::chrono::milliseconds timeout = 500ms) {
        std::unique_lock lock(mutex_);
        return entered_.wait_for(lock, timeout, [this, count] {
            return calls_ >= count;
        });
    }

    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        entered_.notify_all();
    }

    std::size_t calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    bool stopped() const {
        std::lock_guard lock(mutex_);
        return stopped_;
    }

    std::string request_id(std::size_t index) const {
        std::lock_guard lock(mutex_);
        return request_ids_.at(index);
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable entered_;
    const bool block_controls_;
    bool released_{false};
    bool stopped_{false};
    std::size_t calls_{0};
    std::vector<std::string> request_ids_;
};

void control_dispatcher_validates_routes_and_observes_lifecycle() {
    ControlProbeDriver first;
    ControlProbeDriver second;
    ControlCompletionProbe before_start;
    ControlCompletionProbe completions;
    gateway::DeviceControlDispatcher dispatcher{
        4,
        [&first, &second](std::string_view device_id) {
            if (device_id == "device-a") {
                return static_cast<gateway::IProtocolDriver*>(&first);
            }
            if (device_id == "device-b") {
                return static_cast<gateway::IProtocolDriver*>(&second);
            }
            return static_cast<gateway::IProtocolDriver*>(nullptr);
        }};

    CHECK(dispatcher.submit(
              control_request("before", "device-a"),
              before_start.callback()) == gateway::ControlSubmitResult::Stopping);
    CHECK(before_start.size() == 0);

    dispatcher.start();
    bool duplicate_start_rejected = false;
    try {
        dispatcher.start();
    } catch (const std::logic_error&) {
        duplicate_start_rejected = true;
    }
    CHECK(duplicate_start_rejected);

    auto invalid_device = control_request("invalid-device", "device-a");
    invalid_device.device_id.clear();
    CHECK(dispatcher.submit(std::move(invalid_device)) ==
          gateway::ControlSubmitResult::InvalidRequest);
    auto invalid_command = control_request("invalid-command", "device-a");
    invalid_command.command.clear();
    CHECK(dispatcher.submit(std::move(invalid_command)) ==
          gateway::ControlSubmitResult::InvalidRequest);
    CHECK(dispatcher.submit(control_request("unknown", "missing")) ==
          gateway::ControlSubmitResult::UnknownDevice);

    CHECK(dispatcher.submit(
              control_request("request-a", "device-a"),
              completions.callback()) == gateway::ControlSubmitResult::Accepted);
    CHECK(dispatcher.submit(
              control_request("request-b", "device-b"),
              completions.callback()) == gateway::ControlSubmitResult::Accepted);
    CHECK(completions.wait_for(2));

    dispatcher.stop();
    CHECK(dispatcher.submit(control_request("after-stop", "device-a")) ==
          gateway::ControlSubmitResult::Stopping);
    dispatcher.stop();

    CHECK(first.calls() == 1);
    CHECK(second.calls() == 1);
    CHECK(first.request_id(0) == "request-a");
    CHECK(second.request_id(0) == "request-b");
    CHECK(completions.size() == 2);
    CHECK(completions.result(0).status == gateway::DeviceControlStatus::Succeeded);
    CHECK(completions.result(1).status == gateway::DeviceControlStatus::Succeeded);

    const auto stats = dispatcher.stats();
    CHECK(stats.accepted == 2);
    CHECK(stats.executed == 2);
    CHECK(stats.succeeded == 2);
    CHECK(stats.invalid_requests == 2);
    CHECK(stats.unknown_device == 1);
    CHECK(stats.stopping == 2);
}

void control_dispatcher_reports_queue_full_and_completes_once() {
    ControlProbeDriver driver{true};
    ControlCompletionProbe first_completion;
    ControlCompletionProbe second_completion;
    ControlCompletionProbe rejected_completion;
    gateway::DeviceControlDispatcher dispatcher{
        1,
        [&driver](std::string_view device_id) -> gateway::IProtocolDriver* {
            return device_id == "device" ? &driver : nullptr;
        }};
    dispatcher.start();
    CHECK(dispatcher.submit(
              control_request("first"),
              first_completion.callback()) == gateway::ControlSubmitResult::Accepted);
    CHECK(driver.wait_for_calls(1));
    CHECK(dispatcher.submit(
              control_request("second"),
              second_completion.callback()) == gateway::ControlSubmitResult::Accepted);
    CHECK(dispatcher.submit(
              control_request("third"),
              rejected_completion.callback()) == gateway::ControlSubmitResult::QueueFull);

    dispatcher.request_stop();
    CHECK(second_completion.wait_for(1));
    CHECK(second_completion.size() == 1);
    CHECK(second_completion.result(0).status ==
          gateway::DeviceControlStatus::Cancelled);
    CHECK(rejected_completion.size() == 0);

    driver.release();
    dispatcher.join();
    dispatcher.stop();

    CHECK(first_completion.wait_for(1));
    CHECK(first_completion.size() == 1);
    CHECK(first_completion.result(0).status ==
          gateway::DeviceControlStatus::Succeeded);
    CHECK(driver.calls() == 1);

    const auto queue_stats = dispatcher.queue_stats();
    CHECK(queue_stats.size == 0);
    CHECK(queue_stats.capacity == 1);
    CHECK(queue_stats.accepted == 2);
    CHECK(queue_stats.rejected_full == 1);
    CHECK(queue_stats.cancelled == 1);
    const auto stats = dispatcher.stats();
    CHECK(stats.queue_full == 1);
    CHECK(stats.executed == 1);
    CHECK(stats.succeeded == 1);
    CHECK(stats.cancelled == 1);
}

void control_dispatcher_concurrent_stop_is_a_completion_barrier() {
    ControlProbeDriver driver{true};
    ControlCompletionProbe completion;
    gateway::DeviceControlDispatcher dispatcher{
        2,
        [&driver](std::string_view device_id) -> gateway::IProtocolDriver* {
            return device_id == "device" ? &driver : nullptr;
        }};
    dispatcher.start();
    CHECK(dispatcher.submit(
              control_request("blocking"),
              completion.callback()) == gateway::ControlSubmitResult::Accepted);
    CHECK(driver.wait_for_calls(1));

    ConcurrentStopProbe stop_probe;
    std::jthread first_stop{[&] {
        stop_probe.entered();
        dispatcher.stop();
        stop_probe.returned();
    }};
    std::jthread second_stop{[&] {
        stop_probe.entered();
        dispatcher.stop();
        stop_probe.returned();
    }};

    CHECK(stop_probe.wait_for_entered(2));
    std::this_thread::sleep_for(20ms);
    CHECK(stop_probe.returned_count() == 0);

    driver.release();
    first_stop.join();
    second_stop.join();

    CHECK(stop_probe.returned_count() == 2);
    CHECK(completion.wait_for(1));
    CHECK(completion.size() == 1);
    CHECK(driver.calls() == 1);
}

void control_dispatcher_skips_driver_for_expired_deadline() {
    ControlProbeDriver driver;
    ControlCompletionProbe completion;
    gateway::DeviceControlDispatcher dispatcher{
        2,
        [&driver](std::string_view device_id) -> gateway::IProtocolDriver* {
            return device_id == "device" ? &driver : nullptr;
        }};
    dispatcher.start();
    auto request = control_request("expired");
    request.deadline = gateway::ControlClock::now() - 1s;
    CHECK(dispatcher.submit(std::move(request), completion.callback()) ==
          gateway::ControlSubmitResult::Accepted);
    CHECK(completion.wait_for(1));
    dispatcher.stop();

    CHECK(completion.size() == 1);
    CHECK(completion.result(0).request_id == "expired");
    CHECK(completion.result(0).status == gateway::DeviceControlStatus::Timeout);
    CHECK(driver.calls() == 0);
    const auto stats = dispatcher.stats();
    CHECK(stats.executed == 0);
    CHECK(stats.timeouts == 1);
}

class ThrowingControlDriver final : public gateway::IProtocolDriver {
public:
    gateway::DriverCapabilities capabilities() const override {
        return {.mode = gateway::AcquisitionMode::Poll};
    }
    void configure(const gateway::DeviceConfig&, gateway::SampleSink) override {}
    void start() override {}
    void stop() noexcept override {}

    gateway::DeviceControlResult control(
        const gateway::DeviceControlRequest&) override {
        ++calls_;
        throw std::runtime_error{"expected control failure"};
    }

    std::size_t calls_{0};
};

void control_dispatcher_serializes_io_and_maps_driver_errors() {
    ControlProbeDriver driver;
    std::timed_mutex gate;
    gateway::DeviceControlDispatcher dispatcher{
        2,
        [&driver](std::string_view device_id) -> gateway::IProtocolDriver* {
            return device_id == "device" ? &driver : nullptr;
        },
        [&gate](std::string_view device_id) -> std::timed_mutex* {
            return device_id == "device" ? &gate : nullptr;
        }};
    ControlCompletionProbe completion;

    gate.lock();
    dispatcher.start();
    const auto submitted = dispatcher.submit(
        control_request("serialized"), completion.callback());
    std::this_thread::sleep_for(10ms);
    const auto calls_while_locked = driver.calls();
    gate.unlock();
    CHECK(submitted == gateway::ControlSubmitResult::Accepted);
    CHECK(calls_while_locked == 0);
    CHECK(completion.wait_for(1));
    CHECK(driver.calls() == 1);
    CHECK(completion.result(0).status ==
          gateway::DeviceControlStatus::Succeeded);

    ControlCompletionProbe timed_out;
    gate.lock();
    auto expiring = control_request("gate-timeout");
    expiring.deadline = gateway::ControlClock::now() + 20ms;
    const auto expiring_submitted = dispatcher.submit(
        std::move(expiring), timed_out.callback());
    const auto timeout_completed = timed_out.wait_for(1, 200ms);
    gate.unlock();
    CHECK(expiring_submitted == gateway::ControlSubmitResult::Accepted);
    CHECK(timeout_completed);
    CHECK(timed_out.result(0).status == gateway::DeviceControlStatus::Timeout);
    CHECK(driver.calls() == 1);
    dispatcher.stop();

    CHECK(dispatcher.queue_stats().max_size == 1);
    CHECK(dispatcher.stats().execution_ns > 0);

    ThrowingControlDriver throwing_driver;
    gateway::DeviceControlDispatcher throwing_dispatcher{
        1,
        [&throwing_driver](std::string_view) -> gateway::IProtocolDriver* {
            return &throwing_driver;
        }};
    ControlCompletionProbe failed;
    throwing_dispatcher.start();
    CHECK(throwing_dispatcher.submit(
              control_request("failed"),
              failed.callback()) == gateway::ControlSubmitResult::Accepted);
    CHECK(failed.wait_for(1));
    throwing_dispatcher.stop();
    CHECK(throwing_driver.calls_ == 1);
    CHECK(failed.result(0).request_id == "failed");
    CHECK(failed.result(0).status == gateway::DeviceControlStatus::Failed);
    CHECK(failed.result(0).message.find("expected control failure") !=
          std::string::npos);
}

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

void pipeline_injects_nonblocking_control_sink() {
    auto processor = std::make_unique<ControlSubmittingProcessor>();
    auto* observer = processor.get();
    std::vector<std::unique_ptr<gateway::IDataProcessor>> processors;
    processors.push_back(std::move(processor));

    std::size_t submissions = 0;
    gateway::DeviceControlRequest captured;
    gateway::ProcessingPipeline pipeline{
        std::move(processors),
        [&submissions, &captured](
            gateway::DeviceControlRequest&& request,
            gateway::ControlCompletion completion) {
            CHECK(!completion);
            ++submissions;
            captured = std::move(request);
            return gateway::ControlSubmitResult::Accepted;
        }};
    Event event{
        .event_id = "processor-event",
        .device_id = "processor-device",
        .source = "test",
        .readings = {},
        .model_version = {},
    };

    pipeline.process(event);

    CHECK(observer->result_ == gateway::ControlSubmitResult::Accepted);
    CHECK(submissions == 1);
    CHECK(captured.request_id == "processor-request");
    CHECK(captured.device_id == "processor-device");
    CHECK(captured.command == "set");
    CHECK(std::get<bool>(captured.arguments.at("enabled")));
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

class StubControlSource final : public gateway::IDeviceControlSource {
public:
    void configure(gateway::ControlSink submit) override {
        submit_ = std::move(submit);
    }
    void start() override {}
    void request_stop() noexcept override {}
    void stop() noexcept override {}

    gateway::ControlSink submit_;
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
    registry.register_device_control_source(
        "stub-source",
        [](std::string_view)
            -> std::unique_ptr<gateway::IDeviceControlSource> {
            return std::make_unique<StubControlSource>();
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
    config.device_control_sources.push_back(
        {"source", "stub-source", true, "{}", {}});
    config.device_control_sources.push_back(
        {"disabled", "unknown", false, "{}", {}});

    auto instances = registry.create(config);
    CHECK(instances.drivers.size() == 1);
    CHECK(instances.processors.size() == 1);
    CHECK(instances.event_publishers.size() == 1);
    CHECK(instances.sources.size() == 1);
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

class SharedGateProbeDriver final : public gateway::IProtocolDriver {
public:
    gateway::DriverCapabilities capabilities() const override {
        return {.mode = gateway::AcquisitionMode::Poll};
    }

    void configure(
        const gateway::DeviceConfig& device,
        gateway::SampleSink) override {
        device_id_ = device.id;
    }

    void start() override {}
    void stop() noexcept override {
        release_first_poll();
    }

    gateway::RawBatch poll(
        const gateway::CollectionGroup&,
        gateway::TimePoint) override {
        {
            std::unique_lock lock(mutex_);
            ++poll_calls_;
            ++active_io_;
            max_active_io_ = std::max(max_active_io_, active_io_);
            const bool first_poll = poll_calls_ == 1;
            if (first_poll) {
                first_poll_active_ = true;
            }
            condition_.notify_all();
            if (first_poll) {
                static_cast<void>(condition_.wait_for(
                    lock, 5s, [this] { return first_poll_released_; }));
                first_poll_active_ = false;
            }
            --active_io_;
        }
        condition_.notify_all();
        return gateway::RawBatch{
            .device_id = device_id_,
            .source = "shared-gate-probe",
            .samples = {},
        };
    }

    gateway::DeviceControlResult control(
        const gateway::DeviceControlRequest& request) override {
        {
            std::lock_guard lock(mutex_);
            ++control_calls_;
            ++active_io_;
            max_active_io_ = std::max(max_active_io_, active_io_);
            control_overlapped_poll_ =
                control_overlapped_poll_ || first_poll_active_;
            --active_io_;
        }
        condition_.notify_all();
        return gateway::DeviceControlResult{
            .request_id = request.request_id,
            .status = gateway::DeviceControlStatus::Succeeded,
            .outputs = {},
            .message = {},
        };
    }

    bool wait_for_first_poll(
        std::chrono::milliseconds timeout = 500ms) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] {
            return first_poll_active_;
        });
    }

    bool wait_for_control(
        std::chrono::milliseconds timeout = 100ms) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] {
            return control_calls_ > 0;
        });
    }

    void release_first_poll() noexcept {
        {
            std::lock_guard lock(mutex_);
            first_poll_released_ = true;
        }
        condition_.notify_all();
    }

    std::size_t control_calls() const {
        std::lock_guard lock(mutex_);
        return control_calls_;
    }

    std::size_t max_active_io() const {
        std::lock_guard lock(mutex_);
        return max_active_io_;
    }

    bool control_overlapped_poll() const {
        std::lock_guard lock(mutex_);
        return control_overlapped_poll_;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::string device_id_;
    std::size_t poll_calls_{0};
    std::size_t control_calls_{0};
    std::size_t active_io_{0};
    std::size_t max_active_io_{0};
    bool first_poll_active_{false};
    bool first_poll_released_{false};
    bool control_overlapped_poll_{false};
};

class OneShotControlSource final : public gateway::IDeviceControlSource {
public:
    void configure(gateway::ControlSink submit) override {
        if (!submit) {
            throw std::invalid_argument{"control source requires a sink"};
        }
        submit_ = std::move(submit);
        configured_ = true;
    }

    void start() override {
        if (!configured_) {
            throw std::logic_error{"control source is not configured"};
        }
        started_ = true;
        submit_result_ = submit_(
            control_request("source-request", "controlled-device"),
            [this](gateway::DeviceControlResult&& result) {
                {
                    std::lock_guard lock(mutex_);
                    result_ = std::move(result);
                    ++completion_count_;
                }
                condition_.notify_all();
            });
    }

    void request_stop() noexcept override {
        request_stop_called_ = true;
    }

    void stop() noexcept override {
        stop_called_ = true;
    }

    bool wait_for_completion() {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, 500ms, [this] {
            return completion_count_ == 1;
        });
    }

    gateway::DeviceControlResult result() const {
        std::lock_guard lock(mutex_);
        return result_;
    }

    std::size_t completion_count() const {
        std::lock_guard lock(mutex_);
        return completion_count_;
    }

    gateway::ControlSink submit_;
    gateway::ControlSubmitResult submit_result_{
        gateway::ControlSubmitResult::Stopping};
    bool configured_{false};
    bool started_{false};
    bool request_stop_called_{false};
    bool stop_called_{false};

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    gateway::DeviceControlResult result_{};
    std::size_t completion_count_{0};
};

class ShutdownAwareControlSource final
    : public gateway::IDeviceControlSource {
public:
    void configure(gateway::ControlSink submit) override {
        submit_ = std::move(submit);
    }

    void start() override {
        const auto result = submit_(
            control_request("shutdown-source-request", "controlled-device"),
            [this](gateway::DeviceControlResult&& completion) {
                {
                    std::lock_guard lock(mutex_);
                    ++completion_count_;
                    completion_after_request_stop_ = request_stop_called_;
                    completion_before_stop_ = !stop_called_;
                    result_ = std::move(completion);
                }
                condition_.notify_all();
            });
        {
            std::lock_guard lock(mutex_);
            submit_result_ = result;
        }
        condition_.notify_all();
    }

    void request_stop() noexcept override {
        {
            std::lock_guard lock(mutex_);
            request_stop_called_ = true;
        }
        condition_.notify_all();
    }

    void stop() noexcept override {
        {
            std::lock_guard lock(mutex_);
            stop_called_ = true;
            completion_count_at_stop_ = completion_count_;
        }
        condition_.notify_all();
    }

    bool wait_for_request_stop(
        std::chrono::milliseconds timeout = 500ms) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] {
            return request_stop_called_;
        });
    }

    bool wait_for_stop(std::chrono::milliseconds timeout) {
        std::unique_lock lock(mutex_);
        return condition_.wait_for(lock, timeout, [this] {
            return stop_called_;
        });
    }

    gateway::ControlSubmitResult submit_result() const {
        std::lock_guard lock(mutex_);
        return submit_result_;
    }

    gateway::DeviceControlResult result() const {
        std::lock_guard lock(mutex_);
        return result_;
    }

    std::size_t completion_count() const {
        std::lock_guard lock(mutex_);
        return completion_count_;
    }

    std::size_t completion_count_at_stop() const {
        std::lock_guard lock(mutex_);
        return completion_count_at_stop_;
    }

    bool completion_after_request_stop() const {
        std::lock_guard lock(mutex_);
        return completion_after_request_stop_;
    }

    bool completion_before_stop() const {
        std::lock_guard lock(mutex_);
        return completion_before_stop_;
    }

private:
    gateway::ControlSink submit_;
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    gateway::ControlSubmitResult submit_result_{
        gateway::ControlSubmitResult::Stopping};
    gateway::DeviceControlResult result_{};
    std::size_t completion_count_{0};
    std::size_t completion_count_at_stop_{0};
    bool request_stop_called_{false};
    bool stop_called_{false};
    bool completion_after_request_stop_{false};
    bool completion_before_stop_{false};
};

void runtime_runs_generic_control_source_lifecycle() {
    gateway::GatewayConfig config;
    config.raw_queue_capacity = 4;
    config.control_queue_capacity = 4;
    config.devices.push_back(gateway::DeviceConfig{
        .id = "controlled-device",
        .driver = {.type = "test", .library = {}},
        .connection = {},
        .points = {point("value")},
    });

    auto driver = std::make_unique<ControlProbeDriver>();
    auto* driver_observer = driver.get();
    std::vector<gateway::DriverInstance> drivers;
    drivers.push_back({"controlled-device", std::move(driver)});

    auto source = std::make_unique<OneShotControlSource>();
    auto* source_observer = source.get();
    std::vector<std::unique_ptr<gateway::IDeviceControlSource>> sources;
    sources.push_back(std::move(source));

    gateway::GatewayRuntime runtime{
        std::move(config), std::move(drivers), {}, {}, std::move(sources)};
    CHECK(runtime.submit_control(control_request("before-start")) ==
          gateway::ControlSubmitResult::Stopping);

    runtime.start();
    CHECK(source_observer->wait_for_completion());
    CHECK(source_observer->submit_result_ ==
          gateway::ControlSubmitResult::Accepted);
    CHECK(source_observer->result().request_id == "source-request");
    CHECK(source_observer->result().status ==
          gateway::DeviceControlStatus::Succeeded);
    runtime.stop();

    CHECK(source_observer->configured_);
    CHECK(source_observer->started_);
    CHECK(source_observer->request_stop_called_);
    CHECK(source_observer->stop_called_);
    CHECK(source_observer->completion_count() == 1);
    CHECK(driver_observer->calls() == 1);
    CHECK(runtime.submit_control(control_request("after-stop")) ==
          gateway::ControlSubmitResult::Stopping);
    const auto stats = runtime.stats();
    CHECK(stats.control.accepted == 1);
    CHECK(stats.control.succeeded == 1);
}

void runtime_concurrent_stop_waits_for_control_shutdown() {
    gateway::GatewayConfig config;
    config.raw_queue_capacity = 4;
    config.control_queue_capacity = 4;
    config.devices.push_back(gateway::DeviceConfig{
        .id = "controlled-device",
        .driver = {.type = "test", .library = {}},
        .connection = {},
        .points = {point("value")},
    });

    auto driver = std::make_unique<ControlProbeDriver>(true);
    auto* driver_observer = driver.get();
    std::vector<gateway::DriverInstance> drivers;
    drivers.push_back({"controlled-device", std::move(driver)});

    auto source = std::make_unique<OneShotControlSource>();
    auto* source_observer = source.get();
    std::vector<std::unique_ptr<gateway::IDeviceControlSource>> sources;
    sources.push_back(std::move(source));

    gateway::GatewayRuntime runtime{
        std::move(config), std::move(drivers), {}, {}, std::move(sources)};
    runtime.start();
    CHECK(driver_observer->wait_for_calls(1));

    ConcurrentStopProbe stop_probe;
    std::jthread first_stop{[&] {
        stop_probe.entered();
        runtime.stop();
        stop_probe.returned();
    }};
    std::jthread second_stop{[&] {
        stop_probe.entered();
        runtime.stop();
        stop_probe.returned();
    }};

    CHECK(stop_probe.wait_for_entered(2));
    std::this_thread::sleep_for(20ms);
    CHECK(stop_probe.returned_count() == 0);

    driver_observer->release();
    first_stop.join();
    second_stop.join();

    CHECK(stop_probe.returned_count() == 2);
    CHECK(source_observer->completion_count() == 1);
    CHECK(source_observer->request_stop_called_);
    CHECK(source_observer->stop_called_);
    CHECK(driver_observer->stopped());
    CHECK(runtime.submit_control(control_request("after-stop")) ==
          gateway::ControlSubmitResult::Stopping);
}

void runtime_shares_one_device_gate_between_poll_and_control() {
    gateway::GatewayConfig config;
    config.raw_queue_capacity = 8;
    config.control_queue_capacity = 4;
    config.devices.push_back(gateway::DeviceConfig{
        .id = "gated-device",
        .driver = {.type = "test", .library = {}},
        .connection = {},
        .points = {point("value")},
    });
    config.groups.push_back(gateway::CollectionGroup{
        .id = "gated-poll",
        .device_id = "gated-device",
        .interval = 1ms,
        .timeout = 1s,
        .points = {"value"},
    });

    auto driver = std::make_unique<SharedGateProbeDriver>();
    auto* driver_observer = driver.get();
    std::vector<gateway::DriverInstance> drivers;
    drivers.push_back({"gated-device", std::move(driver)});
    ControlCompletionProbe completion;
    gateway::GatewayRuntime runtime{
        std::move(config), std::move(drivers), {}, {}};

    runtime.start();
    const bool poll_entered = driver_observer->wait_for_first_poll();
    auto submit_result = gateway::ControlSubmitResult::Stopping;
    bool control_entered_while_poll_blocked = false;
    if (poll_entered) {
        auto request = control_request("shared-gate", "gated-device");
        request.deadline = gateway::ControlClock::now() + 2s;
        submit_result = runtime.submit_control(
            std::move(request), completion.callback());
        control_entered_while_poll_blocked =
            driver_observer->wait_for_control();
    }

    driver_observer->release_first_poll();
    const bool completed = completion.wait_for(1, 1s);
    runtime.stop();

    CHECK(poll_entered);
    CHECK(submit_result == gateway::ControlSubmitResult::Accepted);
    CHECK(!control_entered_while_poll_blocked);
    CHECK(completed);
    CHECK(completion.result(0).status ==
          gateway::DeviceControlStatus::Succeeded);
    CHECK(driver_observer->control_calls() == 1);
    CHECK(driver_observer->max_active_io() == 1);
    CHECK(!driver_observer->control_overlapped_poll());
}

void runtime_keeps_source_alive_for_completion_during_stop() {
    gateway::GatewayConfig config;
    config.raw_queue_capacity = 4;
    config.control_queue_capacity = 4;
    config.devices.push_back(gateway::DeviceConfig{
        .id = "controlled-device",
        .driver = {.type = "test", .library = {}},
        .connection = {},
        .points = {point("value")},
    });

    auto driver = std::make_unique<ControlProbeDriver>(true);
    auto* driver_observer = driver.get();
    std::vector<gateway::DriverInstance> drivers;
    drivers.push_back({"controlled-device", std::move(driver)});

    auto source = std::make_unique<ShutdownAwareControlSource>();
    auto* source_observer = source.get();
    std::vector<std::unique_ptr<gateway::IDeviceControlSource>> sources;
    sources.push_back(std::move(source));

    gateway::GatewayRuntime runtime{
        std::move(config), std::move(drivers), {}, {}, std::move(sources)};
    runtime.start();
    const bool control_started = driver_observer->wait_for_calls(1);

    std::jthread stopper;
    bool request_stop_seen = false;
    bool source_stopped_while_control_blocked = false;
    if (control_started) {
        stopper = std::jthread([&runtime] { runtime.stop(); });
        request_stop_seen = source_observer->wait_for_request_stop();
        source_stopped_while_control_blocked =
            source_observer->wait_for_stop(100ms);
    }

    driver_observer->release();
    if (stopper.joinable()) {
        stopper.join();
    } else {
        runtime.stop();
    }

    CHECK(control_started);
    CHECK(request_stop_seen);
    CHECK(!source_stopped_while_control_blocked);
    CHECK(source_observer->submit_result() ==
          gateway::ControlSubmitResult::Accepted);
    CHECK(source_observer->completion_count() == 1);
    CHECK(source_observer->completion_count_at_stop() == 1);
    CHECK(source_observer->completion_after_request_stop());
    CHECK(source_observer->completion_before_stop());
    CHECK(source_observer->result().request_id == "shutdown-source-request");
    CHECK(source_observer->result().status ==
          gateway::DeviceControlStatus::Succeeded);
}

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
  "runtime": {"run_duration_ms": 25, "raw_queue_capacity": 4,
    "control_queue_capacity": 3},
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
    "config": {"endpoint": "kept"}}],
  "device_control_sources": [{"id": "custom-control",
    "type": "unrecognized_control_source", "enabled": false,
    "library": "./unrecognized_control_source.dll",
    "config": {"route": "kept"}}]
}
)json";

void config_parser_preserves_generic_plugin_specs() {
    const auto config = gateway::parse_config(valid_config, "config-test");
    CHECK(config.run_duration.has_value());
    CHECK(*config.run_duration == 25ms);
    CHECK(config.gateway.raw_queue_capacity == 4);
    CHECK(config.gateway.control_queue_capacity == 3);
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
    CHECK(config.device_control_sources.size() == 1);
    CHECK(config.device_control_sources.front().type ==
          "unrecognized_control_source");
    CHECK(config.device_control_sources.front().settings_json.find("kept") !=
          std::string::npos);

    std::string bytes_config{valid_config};
    const auto type = bytes_config.find("\"type\": \"double\"");
    CHECK(type != std::string::npos);
    bytes_config.replace(type, std::string{"\"type\": \"double\""}.size(),
                         "\"type\": \"bytes\"");
    const auto parsed_bytes = gateway::parse_config(bytes_config, "bytes-test");
    CHECK(parsed_bytes.gateway.devices.front().points.front().type ==
          gateway::ValueType::ByteArray);

    std::string long_running_config{valid_config};
    constexpr std::string_view duration_field{
        "\"run_duration_ms\": 25, "};
    const auto duration = long_running_config.find(duration_field);
    CHECK(duration != std::string::npos);
    long_running_config.erase(duration, duration_field.size());
    const auto parsed_long_running = gateway::parse_config(
        long_running_config, "long-running-test");
    CHECK(!parsed_long_running.run_duration.has_value());

    std::string null_duration_config{valid_config};
    const auto duration_value = null_duration_config.find(
        "\"run_duration_ms\": 25");
    CHECK(duration_value != std::string::npos);
    null_duration_config.replace(
        duration_value,
        std::string_view{"\"run_duration_ms\": 25"}.size(),
        "\"run_duration_ms\": null");
    const auto parsed_null_duration = gateway::parse_config(
        null_duration_config, "null-duration-test");
    CHECK(!parsed_null_duration.run_duration.has_value());
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
    CHECK(config.run_duration.has_value());
    CHECK(*config.run_duration == 10000ms);
    CHECK(config.gateway.raw_queue_capacity == 32);
    CHECK(config.gateway.control_queue_capacity == 64);
    CHECK(config.gateway.devices.size() == 2);
    CHECK(config.gateway.groups.size() == 2);
    CHECK(config.gateway.devices.front().driver.type == "simulator_poll");
    CHECK(config.gateway.devices.back().driver.type == "simulator_push");
    CHECK(config.processors.size() == 3);
    CHECK(config.event_publishers.size() == 1 ||
          config.event_publishers.size() == 2);
    CHECK(config.event_publishers.front().type == "print");
    if (config.event_publishers.size() == 2) {
        CHECK(config.event_publishers.back().type == "mqtt");
        CHECK(!config.event_publishers.back().enabled);
    }
    CHECK(config.device_control_sources.size() == 1);
    CHECK(config.device_control_sources.front().type == "periodic_control");
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
        {"control dispatcher lifecycle", control_dispatcher_validates_routes_and_observes_lifecycle},
        {"control dispatcher backpressure", control_dispatcher_reports_queue_full_and_completes_once},
        {"control dispatcher concurrent stop", control_dispatcher_concurrent_stop_is_a_completion_barrier},
        {"control dispatcher deadline", control_dispatcher_skips_driver_for_expired_deadline},
        {"control dispatcher serialization", control_dispatcher_serializes_io_and_maps_driver_errors},
        {"normalizer", normalizer_converts_values_and_quality},
        {"normalizer byte arrays", normalizer_preserves_byte_arrays},
        {"pipeline isolation", pipeline_isolates_processor_failures},
        {"pipeline control sink", pipeline_injects_nonblocking_control_sink},
        {"generic plugin registry", registry_creates_only_enabled_generic_plugins},
        {"runtime control source", runtime_runs_generic_control_source_lifecycle},
        {"runtime concurrent stop", runtime_concurrent_stop_waits_for_control_shutdown},
        {"runtime shared device gate", runtime_shares_one_device_gate_between_poll_and_control},
        {"runtime source shutdown completion", runtime_keeps_source_alive_for_completion_during_stop},
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
