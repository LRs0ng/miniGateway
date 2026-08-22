#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace gateway {

// Binary values (for example, a camera snapshot) stay opaque to the core.
// Publishers decide how to encode them for their transport.
using ByteArray = std::vector<std::uint8_t>;
using Scalar = std::variant<std::int64_t, double, bool, std::string, ByteArray>;

enum class ValueType {
    Integer,
    Double,
    Boolean,
    String,
    ByteArray,
};

enum class Quality {
    Good,
    Timeout,
    Disconnected,
    DecodeError,
    OutOfRange,
    Bad,
};

struct PointConfig {
    std::string name;
    ValueType type{ValueType::Double};
    std::string unit;
    std::unordered_map<std::string, std::string> address;
    double scale{1.0};
    double offset{0.0};
    std::optional<double> minimum;
    std::optional<double> maximum;
};

struct PluginSpec {
    std::string type;
    std::string settings_json{"{}"};
    // Complete shared-library filename (including its suffix). Empty is
    // allowed only for in-process test registration.
    std::filesystem::path library;
};

struct DeviceConfig {
    std::string id;
    PluginSpec driver;
    std::unordered_map<std::string, std::string> connection;
    std::vector<PointConfig> points;
};

struct CollectionGroup {
    std::string id;
    std::string device_id;
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds timeout{500};
    std::vector<std::string> points;
};

struct GatewayConfig {
    std::size_t raw_queue_capacity{64};
    std::size_t control_queue_capacity{64};
    std::vector<DeviceConfig> devices;
    std::vector<CollectionGroup> groups;
};

struct RawSample {
    std::string point;
    Scalar value;
    Quality status{Quality::Good};
    std::int64_t source_time_ns{0};
};

struct RawBatch {
    std::string device_id;
    std::string source;
    std::vector<RawSample> samples;
};

struct Reading {
    std::string point;
    Scalar value;
    Quality quality{Quality::Good};
    std::string unit;
    std::int64_t source_time_ns{0};
    std::int64_t received_time_ns{0};
    bool derived{false};
};

struct Event {
    std::string event_id;
    std::string device_id;
    std::string source;
    std::vector<Reading> readings;
    std::string model_version;
};

std::int64_t unix_time_ns();
std::optional<double> numeric_value(const Scalar& value);
std::string scalar_to_string(const Scalar& value);
std::string_view quality_name(Quality quality);

const Reading* find_reading(const Event& event, std::string_view point);
Reading* find_reading(Event& event, std::string_view point);

}  // namespace gateway
