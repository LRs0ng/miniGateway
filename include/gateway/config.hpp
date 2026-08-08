#pragma once

#include "gateway/model.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace gateway {

struct PollSimulatorConfig {
    std::chrono::milliseconds latency{20};
};

struct PushSimulatorConfig {
    std::chrono::milliseconds interval{1000};
};

using DriverSettings =
    std::variant<PollSimulatorConfig, PushSimulatorConfig>;

struct DriverConfig {
    std::string device_id;
    DriverSettings settings;
};

struct ThresholdProcessorConfig {
    std::string input;
    double greater_than{0.0};
    std::string output;
};

struct WindowAverageProcessorConfig {
    std::string input;
    std::size_t window{1};
    std::string output;
};

struct InferenceProcessorConfig {
    std::string runner;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
};

using ProcessorConfig = std::variant<
    ThresholdProcessorConfig,
    WindowAverageProcessorConfig,
    InferenceProcessorConfig>;


struct ApplicationConfig {
    std::chrono::milliseconds run_duration{1000};
    GatewayConfig gateway;
    std::vector<DriverConfig> drivers;
    std::vector<ProcessorConfig> processors;
};

[[nodiscard]] ApplicationConfig parse_config(
    std::string_view json_text,
    std::string_view source_name = "<memory>");

[[nodiscard]] ApplicationConfig load_config(const std::filesystem::path& path);

}  // namespace gateway
