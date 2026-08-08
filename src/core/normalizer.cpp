#include "gateway/normalizer.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace gateway {
namespace {

std::optional<bool> boolean_value(const Scalar& value) {
    return std::visit(
        [](const auto& item) -> std::optional<bool> {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) {
                return item;
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return item != 0;
            } else if constexpr (std::is_same_v<T, double>) {
                return item != 0.0;
            } else {
                if (item == "true" || item == "1") {
                    return true;
                }
                if (item == "false" || item == "0") {
                    return false;
                }
                return std::nullopt;
            }
        },
        value);
}

std::pair<Scalar, Quality> convert_value(
    const Scalar& raw,
    const PointConfig& config,
    Quality quality) {
    if (quality != Quality::Good) {
        return {raw, quality};
    }

    Scalar converted = raw;
    switch (config.type) {
        case ValueType::String:
            converted = scalar_to_string(raw);
            break;
        case ValueType::Boolean: {
            const auto value = boolean_value(raw);
            if (!value) {
                return {raw, Quality::DecodeError};
            }
            converted = *value;
            break;
        }
        case ValueType::Double: {
            const auto value = numeric_value(raw);
            if (!value || !std::isfinite(*value)) {
                return {raw, Quality::DecodeError};
            }
            converted = *value * config.scale + config.offset;
            break;
        }
        case ValueType::Integer: {
            const auto value = numeric_value(raw);
            if (!value || !std::isfinite(*value)) {
                return {raw, Quality::DecodeError};
            }
            const auto scaled = *value * config.scale + config.offset;
            if (scaled < static_cast<double>(std::numeric_limits<std::int64_t>::min()) ||
                scaled > static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
                return {raw, Quality::OutOfRange};
            }
            converted = static_cast<std::int64_t>(std::llround(scaled));
            break;
        }
    }

    const auto numeric = numeric_value(converted);
    if (numeric && ((config.minimum && *numeric < *config.minimum) ||
                    (config.maximum && *numeric > *config.maximum))) {
        quality = Quality::OutOfRange;
    }
    return {std::move(converted), quality};
}

}  // namespace

Normalizer::Normalizer(const GatewayConfig& config) {
    for (const auto& device : config.devices) {
        if (device.id.empty()) {
            throw std::invalid_argument("device id cannot be empty");
        }
        auto [device_it, inserted] = points_.emplace(device.id, PointMap{});
        if (!inserted) {
            throw std::invalid_argument("duplicate device id: " + device.id);
        }
        for (const auto& point : device.points) {
            if (point.name.empty()) {
                throw std::invalid_argument("point name cannot be empty");
            }
            if (point.minimum && point.maximum && *point.minimum > *point.maximum) {
                throw std::invalid_argument("invalid range for point: " + point.name);
            }
            const auto point_inserted = device_it->second.emplace(point.name, point).second;
            if (!point_inserted) {
                throw std::invalid_argument(
                    "duplicate point " + point.name + " in device " + device.id);
            }
        }
    }
}

Event Normalizer::normalize(RawBatch batch) {
    const auto received_at = unix_time_ns();
    const auto sequence = event_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;
    Event event{
        .event_id = "event-" + std::to_string(received_at) + "-" +
                    std::to_string(sequence),
        .device_id = std::move(batch.device_id),
        .source = std::move(batch.source),
        .readings = {},
        .model_version = {},
    };
    event.readings.reserve(batch.samples.size());

    const auto device = points_.find(event.device_id);
    for (auto& sample : batch.samples) {
        Quality quality = sample.status;
        Scalar value = std::move(sample.value);
        std::string unit;

        if (device == points_.end()) {
            quality = Quality::Bad;
        } else if (const auto point = device->second.find(sample.point);
                   point == device->second.end()) {
            quality = Quality::Bad;
        } else {
            unit = point->second.unit;
            auto converted = convert_value(value, point->second, quality);
            value = std::move(converted.first);
            quality = converted.second;
        }

        if (quality != Quality::Good) {
            bad_readings_.fetch_add(1, std::memory_order_relaxed);
        }
        event.readings.push_back(Reading{
            .point = std::move(sample.point),
            .value = std::move(value),
            .quality = quality,
            .unit = std::move(unit),
            .source_time_ns = sample.source_time_ns == 0
                                  ? received_at
                                  : sample.source_time_ns,
            .received_time_ns = received_at,
            .derived = false,
        });
        readings_.fetch_add(1, std::memory_order_relaxed);
    }
    batches_.fetch_add(1, std::memory_order_relaxed);
    return event;
}

NormalizerStats Normalizer::stats() const {
    return NormalizerStats{
        .batches = batches_.load(std::memory_order_relaxed),
        .readings = readings_.load(std::memory_order_relaxed),
        .bad_readings = bad_readings_.load(std::memory_order_relaxed),
    };
}

}  // namespace gateway
