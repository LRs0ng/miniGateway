#pragma once

#include "gateway/model.hpp"

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace gateway {

struct PluginConfig {
    std::string id;
    std::string type;
    bool enabled{true};
    std::string settings_json{"{}"};
    // Complete shared-library filename (including its suffix). Empty is
    // retained only for tests that register a factory directly.
    std::filesystem::path library;
};

struct ApplicationConfig {
    // No duration means that the process runs until it receives a stop signal.
    std::optional<std::chrono::milliseconds> run_duration;
    GatewayConfig gateway;
    std::vector<PluginConfig> processors;
    std::vector<PluginConfig> event_publishers;
    // External control ingress plugins (MQTT, HTTP, IPC, ...).  The core
    // keeps their private `config` JSON opaque and supplies only a ControlSink
    // when the runtime is assembled.
    std::vector<PluginConfig> device_control_sources;
};

[[nodiscard]] ApplicationConfig parse_config(
    std::string_view json_text,
    std::string_view source_name = "<memory>");

[[nodiscard]] ApplicationConfig load_config(const std::filesystem::path& path);

}  // namespace gateway
