#pragma once

#include "gateway/model.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace gateway::processor_support {

inline Reading derived_reading(
    std::string point,
    Scalar value,
    std::int64_t source_time_ns,
    std::int64_t received_time_ns) {
    return Reading{
        .point = std::move(point),
        .value = std::move(value),
        .quality = Quality::Good,
        .unit = {},
        .source_time_ns = source_time_ns,
        .received_time_ns = received_time_ns,
        .derived = true,
    };
}

}  // namespace gateway::processor_support
