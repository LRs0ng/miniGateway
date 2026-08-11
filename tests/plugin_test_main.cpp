#include "gateway/config.hpp"
#include "gateway/model.hpp"
#include "gateway/plugin_registry.hpp"
#include "gateway/processing.hpp"
#include "gateway/runtime.hpp"

#include <chrono>
#include <cmath>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef GATEWAY_TEST_CONFIG_PATH
#define GATEWAY_TEST_CONFIG_PATH "config.json"
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

std::filesystem::path test_config_path() {
    return std::filesystem::path{GATEWAY_TEST_CONFIG_PATH};
}

std::filesystem::path test_plugin_directory() {
    const auto parent = test_config_path().parent_path();
    return parent.empty() ? std::filesystem::current_path() : parent;
}

std::filesystem::path plugin_library(std::string_view target_name) {
#if defined(_WIN32)
    const std::string filename = std::string{target_name} + ".dll";
#elif defined(__APPLE__)
    const std::string filename = "lib" + std::string{target_name} + ".dylib";
#else
    const std::string filename = "lib" + std::string{target_name} + ".so";
#endif
    return test_plugin_directory() / filename;
}

void load_plugins(
    gateway::PluginRegistry& registry,
    const gateway::ApplicationConfig& config) {
    registry.load_dynamic_plugins(config);
}

gateway::PluginConfig processor_config(
    std::string id,
    std::string type,
    std::string library,
    std::string settings) {
    return gateway::PluginConfig{
        .id = std::move(id),
        .type = std::move(type),
        .enabled = true,
        .settings_json = std::move(settings),
        .library = std::filesystem::path{std::move(library)},
    };
}

void processor_plugins_are_created_through_dynamic_libraries() {
    gateway::ApplicationConfig config;
    config.processors = {
        processor_config(
            "alarm", "threshold", plugin_library("gateway_threshold_processor").string(),
            R"({"input":"temperature","greater_than":85,"output":"alarm"})"),
        processor_config(
            "average", "window_average", plugin_library("gateway_window_average_processor").string(),
            R"({"input":"temperature","window":2,"output":"average"})"),
        processor_config(
            "rms", "inference", plugin_library("gateway_inference_processor").string(),
            R"({"runner":"demo_rms","inputs":["x","y"],"outputs":["score"]})"),
    };

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
    auto plugins = registry.create(config);
    CHECK(plugins.processors.size() == 3);

    gateway::ProcessingContext context{.now_ns = 300, .control = {}};
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

void poll_simulator_is_loaded_from_a_shared_library() {
    gateway::ApplicationConfig config;
    gateway::DeviceConfig device{
        .id = "machine",
        .driver = {
            .type = "simulator_poll",
            .settings_json = R"({"latency_ms":0})",
            .library = plugin_library("gateway_poll_simulator"),
        },
        .connection = {},
        .points = {point("temperature")},
    };
    config.gateway.devices.push_back(device);

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
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
    const auto control = driver.control(gateway::DeviceControlRequest{
        .request_id = "poll-control-1",
        .device_id = "machine",
        .command = "print",
        .arguments = {},
        .deadline = gateway::ControlClock::now() + 20ms,
    });
    driver.stop();

    CHECK(batch.device_id == "machine");
    CHECK(batch.source == "simulator_poll");
    CHECK(batch.samples.size() == 1);
    CHECK(batch.samples.front().status == gateway::Quality::Good);
    CHECK(control.request_id == "poll-control-1");
    CHECK(control.status == gateway::DeviceControlStatus::Succeeded);
}

void push_simulator_is_loaded_from_a_shared_library() {
    gateway::ApplicationConfig config;
    gateway::DeviceConfig device{
        .id = "feed",
        .driver = {
            .type = "simulator_push",
            .settings_json = R"({"interval_ms":2})",
            .library = plugin_library("gateway_push_simulator"),
        },
        .connection = {},
        .points = {point("vibration_x")},
    };
    config.gateway.devices.push_back(device);

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
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
    const auto control = driver.control(gateway::DeviceControlRequest{
        .request_id = "push-control-1",
        .device_id = "feed",
        .command = "print",
        .arguments = {},
        .deadline = gateway::ControlClock::now() + 20ms,
    });
    driver.stop();
    const auto stopped_count = received;
    std::this_thread::sleep_for(8ms);
    CHECK(received == stopped_count);
    CHECK(control.request_id == "push-control-1");
    CHECK(control.status == gateway::DeviceControlStatus::Succeeded);
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

void print_publisher_is_loaded_from_a_shared_library() {
    gateway::ApplicationConfig config;
    config.event_publishers.push_back({
        .id = "console",
        .type = "print",
        .enabled = true,
        .settings_json = R"({"include_readings":true})",
        .library = plugin_library("gateway_print_event_publisher"),
    });

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
    auto plugins = registry.create(config);
    CHECK(plugins.event_publishers.size() == 1);
    auto& publisher = *plugins.event_publishers.front().publisher;
    publisher.configure({});
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

#if defined(GATEWAY_HAS_MQTT_PLUGIN)
void mqtt_publisher_handles_an_unavailable_broker() {
    gateway::ApplicationConfig config;
    config.event_publishers.push_back({
        .id = "mqtt",
        .type = "mqtt",
        .enabled = true,
        .settings_json =
            R"({"host":"127.0.0.1","port":1,"keepalive":5,"client_id":"gateway-test","topic_prefix":"edge/events","qos":1,"retain":false,"clean_session":true,"username":"","password":"","reconnect_delay":1,"reconnect_delay_max":2,"reconnect_exponential_backoff":true})",
        .library = plugin_library("gateway_mqtt_event_publisher"),
    });

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
    auto plugins = registry.create(config);
    CHECK(plugins.event_publishers.size() == 1);

    auto& publisher = *plugins.event_publishers.front().publisher;
    publisher.configure({});
    const auto value = event("machine", {reading("temperature", 42.0)});
    for (int attempt = 0; attempt < 3; ++attempt) {
        publisher.start();
        const auto result = publisher.publish(value);
        CHECK(result != gateway::EventPublishResult::Rejected);
        publisher.stop();
        CHECK(
            publisher.publish(value) ==
            gateway::EventPublishResult::Unavailable);
    }
}
#endif

void control_source_is_loaded_from_a_shared_library() {
    gateway::ApplicationConfig config;
    config.device_control_sources.push_back({
        .id = "periodic",
        .type = "periodic_control",
        .enabled = true,
        .settings_json = R"({"request_id_prefix":"request","device_ids":["poll-device","push-device"],"command":"print","interval_ms":5,"timeout_ms":1000})",
        .library = plugin_library("gateway_periodic_control_source"),
    });

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
    auto plugins = registry.create(config);
    CHECK(plugins.sources.size() == 1);

    std::mutex mutex;
    std::condition_variable cv;
    std::vector<gateway::DeviceControlRequest> captured;
    bool first_deadline_was_future = false;
    auto& source = *plugins.sources.front();
    source.configure(
        [&mutex, &cv, &captured, &first_deadline_was_future](
            gateway::DeviceControlRequest&& request,
            gateway::ControlCompletion completion) {
            const auto request_id = request.request_id;
            {
                std::lock_guard lock(mutex);
                if (captured.empty()) {
                    first_deadline_was_future =
                        request.deadline > gateway::ControlClock::now();
                }
                captured.push_back(std::move(request));
            }
            completion(gateway::DeviceControlResult{
                .request_id = request_id,
                .status = gateway::DeviceControlStatus::Succeeded,
                .outputs = {},
                .message = {},
            });
            cv.notify_all();
            return gateway::ControlSubmitResult::Accepted;
        });
    source.start();
    {
        std::unique_lock lock(mutex);
        CHECK(cv.wait_for(lock, 250ms, [&captured] {
            return captured.size() >= 4;
        }));
    }
    source.request_stop();
    source.stop();

    CHECK(captured.size() >= 4);
    CHECK(captured[0].request_id == "request-1");
    CHECK(captured[1].request_id == "request-2");
    CHECK(captured[2].request_id == "request-3");
    CHECK(captured[3].request_id == "request-4");
    CHECK(captured[0].device_id == "poll-device");
    CHECK(captured[1].device_id == "push-device");
    CHECK(captured[2].device_id == "poll-device");
    CHECK(captured[3].device_id == "push-device");
    CHECK(captured[0].command == "print");
    CHECK(first_deadline_was_future);
}

void dynamic_loader_rejects_missing_library_before_runtime_start() {
    gateway::ApplicationConfig config;
    config.processors.push_back({
        .id = "missing",
        .type = "missing",
        .enabled = true,
        .settings_json = "{}",
        .library = plugin_library("does-not-exist"),
    });

    gateway::PluginRegistry registry;
    bool rejected = false;
    try {
        registry.load_dynamic_plugins(config);
    } catch (const std::runtime_error& error) {
        rejected = std::string{error.what()}.find("cannot load plugin") !=
            std::string::npos;
    }
    CHECK(rejected);
}

void dynamic_loader_requires_a_complete_library_filename() {
    const auto exact = plugin_library("gateway_threshold_processor");
    gateway::ApplicationConfig config;
    config.processors.push_back(processor_config(
        "threshold", "threshold",
        (exact.parent_path() / exact.stem()).string(),
        R"({"input":"temperature","greater_than":85,"output":"alarm"})"));

    gateway::PluginRegistry registry;
    bool rejected = false;
    try {
        load_plugins(registry, config);
    } catch (const std::invalid_argument& error) {
        rejected = std::string{error.what()}.find(
            "complete filename with a suffix") != std::string::npos;
    }
    CHECK(rejected);
}

void dynamic_loader_reuses_same_type_with_same_library() {
    const auto library = plugin_library("gateway_threshold_processor");
    gateway::ApplicationConfig config;
    config.processors = {
        processor_config(
            "first", "threshold", library.string(),
            R"({"input":"temperature","greater_than":85,"output":"alarm-a"})"),
        processor_config(
            "second", "threshold", library.string(),
            R"({"input":"temperature","greater_than":90,"output":"alarm-b"})"),
    };

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
    const auto plugins = registry.create(config);
    CHECK(plugins.processors.size() == 2);
}

void dynamic_loader_rejects_cross_category_library_reuse() {
    const auto library = plugin_library("gateway_threshold_processor");
    gateway::ApplicationConfig config;
    config.processors.push_back(processor_config(
        "processor", "threshold", library.string(),
        R"({"input":"temperature","greater_than":85,"output":"alarm"})"));
    config.event_publishers.push_back({
        .id = "publisher",
        .type = "print",
        .enabled = true,
        .settings_json = R"({"include_readings":true})",
        .library = library,
    });

    gateway::PluginRegistry registry;
    bool rejected = false;
    try {
        load_plugins(registry, config);
    } catch (const std::invalid_argument& error) {
        rejected = std::string{error.what()}.find(
            "already assigned") != std::string::npos;
    }
    CHECK(rejected);
}

void json_assembles_plugins_end_to_end() {
    auto config = gateway::load_config(test_config_path());
    // Cover the 300 ms poll interval with margin, while staying below the
    // 500 ms control interval so only the initial request is deterministic.
    config.run_duration = 450ms;

    gateway::PluginRegistry registry;
    load_plugins(registry, config);
    auto plugins = registry.create(config);
    CHECK(plugins.drivers.size() == 2);
    CHECK(plugins.processors.size() == 3);
    CHECK(plugins.event_publishers.size() == 1);
    CHECK(plugins.sources.size() == 1);

    gateway::GatewayRuntime runtime{
        std::move(config.gateway),
        std::move(plugins.drivers),
        std::move(plugins.processors),
        std::move(plugins.event_publishers),
        std::move(plugins.sources)};
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
    CHECK(stats.control.accepted > 0);
    CHECK(stats.control.executed == stats.control.accepted);
    CHECK(stats.control.succeeded == stats.control.accepted);
}

struct TestCase {
    const char* name;
    void (*run)();
};

}  // namespace

int main() {
    const std::vector<TestCase> tests{
        {"processor dynamic plugins", processor_plugins_are_created_through_dynamic_libraries},
        {"poll simulator dynamic plugin", poll_simulator_is_loaded_from_a_shared_library},
        {"push simulator dynamic plugin", push_simulator_is_loaded_from_a_shared_library},
        {"print publisher dynamic plugin", print_publisher_is_loaded_from_a_shared_library},
#if defined(GATEWAY_HAS_MQTT_PLUGIN)
        {"MQTT publisher unavailable broker", mqtt_publisher_handles_an_unavailable_broker},
#endif
        {"control source dynamic plugin", control_source_is_loaded_from_a_shared_library},
        {"missing library", dynamic_loader_rejects_missing_library_before_runtime_start},
        {"library filename validation", dynamic_loader_requires_a_complete_library_filename},
        {"same-type library reuse", dynamic_loader_reuses_same_type_with_same_library},
        {"cross-category library reuse", dynamic_loader_rejects_cross_category_library_reuse},
        {"JSON dynamic assembly", json_assembles_plugins_end_to_end},
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
