#include "gateway/config.hpp"
#include "gateway/runtime.hpp"
#include "gateway/simulator.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <variant>
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

std::filesystem::path default_config_path() {
    const std::filesystem::path current_directory_config{"config.json"};
    {
        std::ifstream input{current_directory_config, std::ios::binary};
        if (input.is_open()) {
            return current_directory_config;
        }
    }

    // Make the source-tree invocation convenient without depending on argv[0]
    // encoding, which is not reliable for non-ASCII Windows working paths.
    const std::filesystem::path source_tree_config{"example/config.json"};
    {
        std::ifstream input{source_tree_config, std::ios::binary};
        if (input.is_open()) {
            return source_tree_config;
        }
    }
    return current_directory_config;
}

std::vector<gateway::DriverInstance> make_drivers(
    const std::vector<gateway::DriverConfig>& configs) {
    std::vector<gateway::DriverInstance> drivers;
    drivers.reserve(configs.size());
    for (const auto& config : configs) {
        std::unique_ptr<gateway::IProtocolDriver> driver;
        if (const auto* poll =
                std::get_if<gateway::PollSimulatorConfig>(&config.settings)) {
            driver = std::make_unique<gateway::PollSimulatorDriver>(poll->latency);
        } else if (const auto* push =
                       std::get_if<gateway::PushSimulatorConfig>(&config.settings)) {
            driver = std::make_unique<gateway::PushSimulatorDriver>(push->interval);
        } else {
            throw std::logic_error{"unsupported driver configuration"};
        }
        drivers.push_back(gateway::DriverInstance{
            .device_id = config.device_id,
            .driver = std::move(driver),
        });
    }
    return drivers;
}

std::vector<std::unique_ptr<gateway::IDataProcessor>> make_processors(
    const std::vector<gateway::ProcessorConfig>& configs) {
    std::vector<std::unique_ptr<gateway::IDataProcessor>> processors;
    processors.reserve(configs.size());
    for (const auto& config : configs) {
        if (const auto* threshold =
                std::get_if<gateway::ThresholdProcessorConfig>(&config)) {
            processors.push_back(std::make_unique<gateway::ThresholdProcessor>(
                threshold->input, threshold->greater_than, threshold->output));
        } else if (const auto* average =
                       std::get_if<gateway::WindowAverageProcessorConfig>(&config)) {
            processors.push_back(std::make_unique<gateway::WindowAverageProcessor>(
                average->input, average->window, average->output));
        } else if (const auto* inference =
                       std::get_if<gateway::InferenceProcessorConfig>(&config)) {
            if (inference->runner != "demo_rms") {
                throw std::logic_error{"unsupported model runner configuration"};
            }
            processors.push_back(std::make_unique<gateway::InferenceProcessor>(
                inference->inputs,
                inference->outputs,
                std::make_shared<DemoModelRunner>()));
        } else {
            throw std::logic_error{"unsupported processor configuration"};
        }
    }
    return processors;
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

    try {
        const auto config_path = default_config_path();
        auto config = gateway::load_config(config_path);
        auto drivers = make_drivers(config.drivers);
        auto processors = make_processors(config.processors);
        const auto run_for = config.run_duration;

        gateway::GatewayRuntime runtime{
            std::move(config.gateway), std::move(drivers), std::move(processors), print_event};
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
