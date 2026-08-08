#include "bootstrap/linked_plugins.hpp"

#include "gateway/config.hpp"
#include "gateway/model.hpp"
#include "gateway/plugin_registry.hpp"
#include "gateway/processing.hpp"
#include "gateway/runtime.hpp"
#include "print_event_publisher.hpp"
#include "processor/processors.hpp"
#include "simulator.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
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

gateway::PointConfig point(std::string name) {
    gateway::PointConfig config;
    config.name = std::move(name);
    config.type = gateway::ValueType::Double;
    return config;
}

gateway::Reading reading(std::string point_name, gateway::Scalar value) {
    return gateway::Reading{
        .point = std::move(point_name),
        .value = std::move(value),
        .quality = gateway::Quality::Good,
        .unit = {},
        .source_time_ns = 100,
        .received_time_ns = 200,
        .derived = false,
    };
}

gateway::Event event(
    std::string device_id,
    std::vector<gateway::Reading> readings) {
    return gateway::Event{
        .event_id = "event-1",
        .device_id = std::move(device_id),
        .source = "test",
        .readings = std::move(readings),
        .model_version = {},
    };
}

void processor_plugins_are_created_through_the_common_interface() {
    gateway::PluginRegistry registry;
    gateway::register_threshold_processor_plugin(registry);
    gateway::register_window_average_processor_plugin(registry);
    gateway::register_inference_processor_plugin(registry);

    gateway::ApplicationConfig config;
    config.processors = {
        {"alarm", "threshold", true,
         R"({"input":"temperature","greater_than":85,"output":"alarm"})"},
        {"average", "window_average", true,
         R"({"input":"temperature","window":2,"output":"average"})"},
        {"rms", "inference", true,
         R"({"runner":"demo_rms","inputs":["x","y"],"outputs":["score"]})"},
    };
    auto plugins = registry.create(config);
    CHECK(plugins.processors.size() == 3);
    gateway::ProcessingContext context{.now_ns = 300};
    auto first = event(
        "machine",
        {reading("temperature", 90.0), reading("x", 3.0), reading("y", 4.0)});
    for (auto& processor : plugins.processors) {
        processor->process(first, context);
    }
    const auto* alarm = gateway::find_reading(first, "alarm");
    CHECK(alarm != nullptr);
    CHECK(std::get<bool>(alarm->value));
    CHECK(gateway::find_reading(first, "average") == nullptr);
    const auto* score = gateway::find_reading(first, "score");
    CHECK(score != nullptr);
    check_near(std::get<double>(score->value), std::sqrt(12.5));
    CHECK(first.model_version == "demo-rms-v1");

    auto second = event("machine", {reading("temperature", 80.0)});
    plugins.processors[1]->process(second, context);
    const auto* average = gateway::find_reading(second, "average");
    CHECK(average != nullptr);
    check_near(std::get<double>(average->value), 85.0);
}

void poll_simulator_is_a_driver_plugin() {
    gateway::PluginRegistry registry;
    gateway::register_poll_simulator_plugin(registry);
    gateway::ApplicationConfig config;
    gateway::DeviceConfig device{
        .id = "machine",
        .driver = {
            .type = "simulator_poll",
            .settings_json = R"({"latency_ms":0})",
        },
        .connection = {},
        .points = {point("temperature")},
    };
    config.gateway.devices.push_back(device);
    auto plugins = registry.create(config);
    CHECK(plugins.drivers.size() == 1);

    auto& driver = *plugins.drivers.front().driver;
    CHECK(driver.capabilities().mode == gateway::AcquisitionMode::Poll);
    driver.configure(device, {});
    const gateway::CollectionGroup group{
        .id = "temperatures",
        .device_id = "machine",
        .interval = 20ms,
        .timeout = 10ms,
        .points = {"temperature"},
    };
    driver.start();
    const auto batch = driver.poll(
        group, gateway::SchedulerClock::now() + 20ms);
    driver.stop();

    CHECK(batch.device_id == "machine");
    CHECK(batch.source == "simulator_poll");
    CHECK(batch.samples.size() == 1);
    CHECK(batch.samples.front().status == gateway::Quality::Good);
}

void push_simulator_is_a_driver_plugin() {
    gateway::PluginRegistry registry;
    gateway::register_push_simulator_plugin(registry);
    gateway::ApplicationConfig config;
    gateway::DeviceConfig device{
        .id = "feed",
        .driver = {
            .type = "simulator_push",
            .settings_json = R"({"interval_ms":2})",
        },
        .connection = {},
        .points = {point("vibration_x")},
    };
    config.gateway.devices.push_back(device);
    auto plugins = registry.create(config);
    CHECK(plugins.drivers.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    std::size_t received = 0;
    auto& driver = *plugins.drivers.front().driver;
    CHECK(driver.capabilities().mode == gateway::AcquisitionMode::Push);
    driver.configure(
        device,
        [&](gateway::RawBatch&& batch) {
            CHECK(batch.source == "simulator_push");
            {
                std::lock_guard lock(mutex);
                ++received;
            }
            cv.notify_all();
            return gateway::EnqueueResult::Accepted;
        });
    driver.start();
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, 200ms, [&] { return received >= 2; }));
    }
    driver.stop();
    const auto stopped_count = received;
    std::this_thread::sleep_for(8ms);
    CHECK(received == stopped_count);
}

class CoutCapture {
public:
    CoutCapture() : previous_(std::cout.rdbuf(stream_.rdbuf())) {}
    ~CoutCapture() {
        std::cout.rdbuf(previous_);
    }

    [[nodiscard]] std::string text() const {
        return stream_.str();
    }

private:
    std::ostringstream stream_;
    std::streambuf* previous_;
};

void print_publisher_replaces_the_main_callback() {
    gateway::PluginRegistry registry;
    gateway::register_print_event_publisher_plugin(registry);
    gateway::ApplicationConfig config;
    config.event_publishers.push_back(
        {"console", "print", true, R"({"include_readings":true})"});
    auto plugins = registry.create(config);
    CHECK(plugins.event_publishers.size() == 1);
    auto& publisher = *plugins.event_publishers.front().publisher;
    gateway::GatewayConfig gateway_config;
    publisher.configure(gateway_config);
    publisher.start();
    gateway::Event value = event("machine", {reading("temperature", 42.0)});
    value.model_version = "model-v1";
    {
        CoutCapture capture;
        CHECK(publisher.publish(value) == gateway::EventPublishResult::Accepted);
        const auto output = capture.text();
        CHECK(output.find("device=machine") != std::string::npos);
        CHECK(output.find("temperature=42.000") != std::string::npos);
        CHECK(output.find("model=model-v1") != std::string::npos);
    }
    publisher.stop();
    CHECK(publisher.publish(value) == gateway::EventPublishResult::Unavailable);
}


void json_assembles_all_linked_plugins_end_to_end() {
    auto config = gateway::load_config(GATEWAY_TEST_CONFIG_PATH);
    config.run_duration = 500ms;

    gateway::PluginRegistry registry;
    register_linked_plugins(registry);
    auto plugins = registry.create(config);
    CHECK(plugins.drivers.size() == 2);
    CHECK(plugins.processors.size() == 3);
    CHECK(plugins.event_publishers.size() == 1);

    gateway::GatewayRuntime runtime{
        std::move(config.gateway),
        std::move(plugins.drivers),
        std::move(plugins.processors),
        std::move(plugins.event_publishers)};
    {
        CoutCapture capture;
        runtime.start();
        std::this_thread::sleep_for(config.run_duration);
        runtime.stop();
        CHECK(capture.text().find("device=") != std::string::npos);
    }

    const auto stats = runtime.stats();
    CHECK(stats.delivered_events > 0);
    CHECK(stats.acquisition.polls > 0);
    CHECK(stats.processing.events == stats.delivered_events);
    CHECK(stats.event_publishers.attempts == stats.delivered_events);
    CHECK(stats.event_publishers.accepted == stats.delivered_events);
    CHECK(stats.event_publishers.errors == 0);
}

struct TestCase {
    const char* name;
    void (*run)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"processor plugins", processor_plugins_are_created_through_the_common_interface},
        {"poll simulator plugin", poll_simulator_is_a_driver_plugin},
        {"push simulator plugin", push_simulator_is_a_driver_plugin},
        {"print publisher plugin", print_publisher_replaces_the_main_callback},
        {"JSON plugin assembly", json_assembles_all_linked_plugins_end_to_end},
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
