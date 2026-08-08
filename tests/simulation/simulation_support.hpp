#pragma once

#include "gateway/model.hpp"

#include <algorithm>
#include <cstdint>
#include <string_view>

namespace gateway::simulation_support {

inline bool has_point(const DeviceConfig& device, std::string_view name) {
    return std::any_of(
        device.points.begin(), device.points.end(),
        [name](const PointConfig& point) { return point.name == name; });
}

inline double simulated_value(
    std::string_view point,
    std::uint64_t sequence) {
    if (point.find("temperature") != std::string_view::npos) {
        return 78.0 + static_cast<double>(sequence % 8) * 2.0;
    }
    if (point.find("pressure") != std::string_view::npos) {
        return 100.0 + static_cast<double>(sequence % 5) * 0.5;
    }
    if (point.find("vibration_x") != std::string_view::npos) {
        return 0.2 + static_cast<double>(sequence % 7) * 0.1;
    }
    if (point.find("vibration_y") != std::string_view::npos) {
        return 0.4 + static_cast<double>(sequence % 5) * 0.12;
    }
    return static_cast<double>(sequence);
}

}  // namespace gateway::simulation_support
