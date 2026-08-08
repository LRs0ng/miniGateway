#pragma once

#include "gateway/model.hpp"

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace gateway {

struct PluginConfig {
    std::string id;
    std::string type;
    bool enabled{true};
    std::string settings_json{"{}"};
};

struct ApplicationConfig {
    std::chrono::milliseconds run_duration{1000};
    GatewayConfig gateway;
    std::vector<PluginConfig> processors;
    std::vector<PluginConfig> event_publishers;
};

[[nodiscard]] ApplicationConfig parse_config(
    std::string_view json_text,
    std::string_view source_name = "<memory>");

[[nodiscard]] ApplicationConfig load_config(const std::filesystem::path& path);

}  // namespace gateway
