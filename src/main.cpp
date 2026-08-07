#include "gateway/runtime.hpp"
#include "gateway/simulator.hpp"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace {

class DemoModelRunner final : public gateway::IModelRunner {
public:
    std::string_view version() const noexcept override {
        return "demo-rms-v1";
    }

    std::vector<double> run(std::span<const double> inputs) override {
        double squared_sum = 0.0;
        for (const auto input : inputs) {
            squared_sum += input * input;
        }
        return {std::sqrt(squared_sum / static_cast<double>(inputs.size()))};
    }
};

gateway::GatewayConfig make_config() {
    using namespace std::chrono_literals;

    gateway::GatewayConfig config;
    config.raw_queue_capacity = 32;
    config.devices = {
        gateway::DeviceConfig{
            .id = "machine-01",
            .driver = "simulator_poll",
            .connection = {},
            .points = {
                gateway::PointConfig{
                    .name = "temperature",
                    .type = gateway::ValueType::Double,
                    .unit = "C",
                    .address = {},
                    .scale = 1.0,
                    .offset = 0.0,
                    .minimum = -40.0,
                    .maximum = 120.0,
                },
                gateway::PointConfig{
                    .name = "pressure",
                    .type = gateway::ValueType::Double,
                    .unit = "kPa",
                    .address = {},
                    .scale = 1.0,
                    .offset = 0.0,
                    .minimum = std::nullopt,
                    .maximum = std::nullopt,
                },
            },
        },
        gateway::DeviceConfig{
            .id = "vibration-feed",
            .driver = "simulator_push",
            .connection = {},
            .points = {
                gateway::PointConfig{
                    .name = "vibration_x",
                    .type = gateway::ValueType::Double,
                    .unit = "g",
                    .address = {},
                    .scale = 1.0,
                    .offset = 0.0,
                    .minimum = std::nullopt,
                    .maximum = std::nullopt,
                },
                gateway::PointConfig{
                    .name = "vibration_y",
                    .type = gateway::ValueType::Double,
                    .unit = "g",
                    .address = {},
                    .scale = 1.0,
                    .offset = 0.0,
                    .minimum = std::nullopt,
                    .maximum = std::nullopt,
                },
            },
        },
    };
    config.groups = {
        gateway::CollectionGroup{
            .id = "temperature-group",
            .device_id = "machine-01",
            .interval = 300ms,
            .timeout = 150ms,
            .points = {"temperature"},
        },
        gateway::CollectionGroup{
            .id = "pressure-group",
            .device_id = "machine-01",
            .interval = 300ms,
            .timeout = 150ms,
            .points = {"pressure"},
        },
    };
    return config;
}

void print_event(const gateway::Event& event) {
    std::cout << event.event_id << " device=" << event.device_id
              << " source=" << event.source;
    if (!event.model_version.empty()) {
        std::cout << " model=" << event.model_version;
    }
    std::cout << '\n';

    for (const auto& reading : event.readings) {
        std::cout << "  " << (reading.derived ? "[derived] " : "")
                  << reading.point << '=' << gateway::scalar_to_string(reading.value);
        if (!reading.unit.empty()) {
            std::cout << ' ' << reading.unit;
        }
        std::cout << " quality=" << gateway::quality_name(reading.quality) << '\n';
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace std::chrono_literals;

    std::chrono::milliseconds run_for{2200};
    if (argc == 2) {
        try {
            run_for = std::chrono::milliseconds{std::stoll(argv[1])};
        } catch (...) {
            std::cerr << "usage: gateway_example [run_duration_ms]\n";
            return EXIT_FAILURE;
        }
        if (run_for <= 0ms) {
            std::cerr << "run_duration_ms must be positive\n";
            return EXIT_FAILURE;
        }
    }

    auto config = make_config();
    std::vector<gateway::DriverInstance> drivers;
    drivers.push_back(gateway::DriverInstance{
        .device_id = "machine-01",
        .driver = std::make_unique<gateway::PollSimulatorDriver>(35ms),
    });
    drivers.push_back(gateway::DriverInstance{
        .device_id = "vibration-feed",
        .driver = std::make_unique<gateway::PushSimulatorDriver>(180ms),
    });

    std::vector<std::unique_ptr<gateway::IDataProcessor>> processors;
    processors.push_back(std::make_unique<gateway::ThresholdProcessor>(
        "temperature", 85.0, "temperature_alarm"));
    processors.push_back(std::make_unique<gateway::WindowAverageProcessor>(
        "vibration_x", 3, "vibration_x_avg"));
    processors.push_back(std::make_unique<gateway::InferenceProcessor>(
        std::vector<std::string>{"vibration_x", "vibration_y"},
        std::vector<std::string>{"anomaly_score"},
        std::make_shared<DemoModelRunner>()));

    try {
        gateway::GatewayRuntime runtime{
            std::move(config), std::move(drivers), std::move(processors), print_event};
        runtime.start();
        std::this_thread::sleep_for(run_for);
        runtime.stop();

        const auto stats = runtime.stats();
        std::cout << "\nsummary: events=" << stats.delivered_events
                  << " queue_full=" << stats.raw_queue.rejected_full
                  << " polls=" << stats.executor.polls
                  << " poll_errors=" << stats.scheduler.errors
                  << " skipped=" << stats.scheduler.skipped_cycles
                  << " max_active_poll=" << stats.scheduler.max_active_poll
                  << " rule_errors=" << stats.processing.processor_errors << '\n';
    } catch (const std::exception& error) {
        std::cerr << "gateway failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
