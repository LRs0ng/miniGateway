#include "gateway/config.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace gateway {
namespace {

using Json = nlohmann::json;

class ConfigError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(std::string_view path, std::string_view message) {
    throw ConfigError{std::string{path} + ": " + std::string{message}};
}

const Json& member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    if (!object.is_object()) {
        fail(path, "must be an object");
    }
    const auto item = object.find(key);
    if (item == object.end()) {
        fail(
            std::string{path} + "." + std::string{key},
            "is a required field");
    }
    return *item;
}

const Json& object_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    const auto& value = member(object, key, path);
    if (!value.is_object()) {
        fail(std::string{path} + "." + std::string{key}, "must be an object");
    }
    return value;
}

const Json& array_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    const auto& value = member(object, key, path);
    if (!value.is_array()) {
        fail(std::string{path} + "." + std::string{key}, "must be an array");
    }
    return value;
}

std::string string_value(
    const Json& value,
    std::string_view path,
    bool allow_empty = false) {
    if (!value.is_string()) {
        fail(path, "must be a string");
    }
    auto result = value.get<std::string>();
    if (!allow_empty && result.empty()) {
        fail(path, "must not be empty");
    }
    return result;
}

std::string string_member(
    const Json& object,
    std::string_view key,
    std::string_view path,
    bool allow_empty = false) {
    return string_value(
        member(object, key, path),
        std::string{path} + "." + std::string{key},
        allow_empty);
}

bool optional_enabled(const Json& object, std::string_view path) {
    const auto item = object.find("enabled");
    if (item == object.end()) {
        return true;
    }
    if (!item->is_boolean()) {
        fail(std::string{path} + ".enabled", "must be a boolean");
    }
    return item->get<bool>();
}

std::uint64_t unsigned_integer_value(
    const Json& value,
    std::string_view path) {
    if (!value.is_number_integer()) {
        fail(path, "must be a non-negative integer");
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    const auto signed_value = value.get<std::int64_t>();
    if (signed_value < 0) {
        fail(path, "must be a non-negative integer");
    }
    return static_cast<std::uint64_t>(signed_value);
}

std::uint64_t unsigned_integer_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    return unsigned_integer_value(
        member(object, key, path),
        std::string{path} + "." + std::string{key});
}

std::size_t size_member(
    const Json& object,
    std::string_view key,
    std::string_view path,
    bool positive) {
    const auto field_path = std::string{path} + "." + std::string{key};
    const auto value = unsigned_integer_member(object, key, path);
    if ((positive && value == 0) ||
        value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
        fail(field_path, positive ? "must be a positive size" : "is too large");
    }
    return static_cast<std::size_t>(value);
}

std::chrono::milliseconds milliseconds_member(
    const Json& object,
    std::string_view key,
    std::string_view path,
    bool positive) {
    using Rep = std::chrono::milliseconds::rep;
    const auto field_path = std::string{path} + "." + std::string{key};
    const auto value = unsigned_integer_member(object, key, path);
    if ((positive && value == 0) ||
        value > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
        fail(field_path, positive ? "must be a positive duration" : "is too large");
    }
    return std::chrono::milliseconds{static_cast<Rep>(value)};
}

double finite_number_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    const auto& value = member(object, key, path);
    const auto field_path = std::string{path} + "." + std::string{key};
    if (!value.is_number()) {
        fail(field_path, "must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        fail(field_path, "must be finite");
    }
    return result;
}

std::optional<double> optional_number_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    const auto item = object.find(key);
    if (item == object.end() || item->is_null()) {
        return std::nullopt;
    }
    const auto field_path = std::string{path} + "." + std::string{key};
    if (!item->is_number()) {
        fail(field_path, "must be a number or null");
    }
    const auto result = item->get<double>();
    if (!std::isfinite(result)) {
        fail(field_path, "must be finite");
    }
    return result;
}

std::unordered_map<std::string, std::string> string_map(
    const Json& value,
    std::string_view path) {
    if (!value.is_object()) {
        fail(path, "must be an object containing string values");
    }
    std::unordered_map<std::string, std::string> result;
    for (const auto& [key, item] : value.items()) {
        if (!item.is_string()) {
            fail(std::string{path} + "." + key, "must be a string");
        }
        result.emplace(key, item.get<std::string>());
    }
    return result;
}

std::vector<std::string> string_array(
    const Json& value,
    std::string_view path,
    bool require_non_empty) {
    if (!value.is_array() || (require_non_empty && value.empty())) {
        fail(path, require_non_empty ? "must be a non-empty array" : "must be an array");
    }

    std::vector<std::string> result;
    std::unordered_set<std::string> unique;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto item_path = std::string{path} + "[" +
            std::to_string(index) + "]";
        auto item = string_value(value[index], item_path);
        if (!unique.insert(item).second) {
            fail(item_path, "must be unique within the array");
        }
        result.push_back(std::move(item));
    }
    return result;
}

ValueType value_type(const Json& value, std::string_view path) {
    const auto name = string_value(value, path);
    if (name == "integer") {
        return ValueType::Integer;
    }
    if (name == "double") {
        return ValueType::Double;
    }
    if (name == "boolean") {
        return ValueType::Boolean;
    }
    if (name == "string") {
        return ValueType::String;
    }
    fail(path, "unsupported value type '" + name + "'");
}

PointConfig parse_point(const Json& value, std::string_view path) {
    if (!value.is_object()) {
        fail(path, "must be an object");
    }

    PointConfig point;
    point.name = string_member(value, "name", path);
    point.type = value_type(
        member(value, "type", path), std::string{path} + ".type");
    point.unit = string_member(value, "unit", path, true);
    point.address = string_map(
        member(value, "address", path), std::string{path} + ".address");
    point.scale = finite_number_member(value, "scale", path);
    point.offset = finite_number_member(value, "offset", path);
    point.minimum = optional_number_member(value, "minimum", path);
    point.maximum = optional_number_member(value, "maximum", path);
    if (point.minimum && point.maximum && *point.minimum > *point.maximum) {
        fail(path, "minimum must not exceed maximum");
    }
    return point;
}

void parse_devices(
    const Json& root,
    ApplicationConfig& config,
    std::unordered_map<std::string, std::unordered_set<std::string>>& points) {
    const auto& devices = array_member(root, "devices", "root");
    if (devices.empty()) {
        fail("root.devices", "must not be empty");
    }

    std::unordered_set<std::string> device_ids;
    config.gateway.devices.reserve(devices.size());
    for (std::size_t index = 0; index < devices.size(); ++index) {
        const auto path = "root.devices[" + std::to_string(index) + "]";
        const auto& value = devices[index];
        if (!value.is_object()) {
            fail(path, "must be an object");
        }

        DeviceConfig device;
        device.id = string_member(value, "id", path);
        if (!device_ids.insert(device.id).second) {
            fail(path + ".id", "duplicate device id '" + device.id + "'");
        }
        device.driver.type = string_member(value, "driver", path);
        device.driver.settings_json =
            object_member(value, "driver_config", path).dump();
        device.connection = string_map(
            member(value, "connection", path), path + ".connection");

        const auto& point_values = array_member(value, "points", path);
        if (point_values.empty()) {
            fail(path + ".points", "must not be empty");
        }
        auto& point_names = points[device.id];
        device.points.reserve(point_values.size());
        for (std::size_t point_index = 0;
             point_index < point_values.size();
             ++point_index) {
            const auto point_path = path + ".points[" +
                std::to_string(point_index) + "]";
            auto point = parse_point(point_values[point_index], point_path);
            if (!point_names.insert(point.name).second) {
                fail(point_path + ".name", "duplicate point '" + point.name + "'");
            }
            device.points.push_back(std::move(point));
        }
        config.gateway.devices.push_back(std::move(device));
    }
}

void parse_groups(
    const Json& root,
    ApplicationConfig& config,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& points) {
    const auto& groups = array_member(root, "groups", "root");
    std::unordered_set<std::string> group_ids;
    config.gateway.groups.reserve(groups.size());
    for (std::size_t index = 0; index < groups.size(); ++index) {
        const auto path = "root.groups[" + std::to_string(index) + "]";
        const auto& value = groups[index];
        if (!value.is_object()) {
            fail(path, "must be an object");
        }

        CollectionGroup group;
        group.id = string_member(value, "id", path);
        if (!group_ids.insert(group.id).second) {
            fail(path + ".id", "duplicate group id '" + group.id + "'");
        }
        group.device_id = string_member(value, "device_id", path);
        group.interval = milliseconds_member(value, "interval_ms", path, true);
        group.timeout = milliseconds_member(value, "timeout_ms", path, true);
        group.points = string_array(
            member(value, "points", path), path + ".points", true);

        const auto device_points = points.find(group.device_id);
        if (device_points == points.end()) {
            fail(path + ".device_id", "references an unknown device");
        }
        for (const auto& point : group.points) {
            if (!device_points->second.contains(point)) {
                fail(path + ".points", "references unknown point '" + point + "'");
            }
        }
        config.gateway.groups.push_back(std::move(group));
    }
}

std::vector<PluginConfig> parse_plugins(
    const Json& root,
    std::string_view key) {
    const auto& values = array_member(root, key, "root");
    const auto base_path = "root." + std::string{key};
    std::unordered_set<std::string> ids;
    std::vector<PluginConfig> result;
    result.reserve(values.size());

    for (std::size_t index = 0; index < values.size(); ++index) {
        const auto path = base_path + "[" + std::to_string(index) + "]";
        const auto& value = values[index];
        if (!value.is_object()) {
            fail(path, "must be an object");
        }
        PluginConfig plugin{
            .id = string_member(value, "id", path),
            .type = string_member(value, "type", path),
            .enabled = optional_enabled(value, path),
            .settings_json = object_member(value, "config", path).dump(),
        };
        if (!ids.insert(plugin.id).second) {
            fail(path + ".id", "duplicate plugin id '" + plugin.id + "'");
        }
        result.push_back(std::move(plugin));
    }
    return result;
}

ApplicationConfig parse_application_config(const Json& root) {
    if (!root.is_object()) {
        fail("root", "must be an object");
    }

    ApplicationConfig config;
    const auto& runtime = object_member(root, "runtime", "root");
    config.run_duration = milliseconds_member(
        runtime, "run_duration_ms", "root.runtime", true);
    config.gateway.raw_queue_capacity = size_member(
        runtime, "raw_queue_capacity", "root.runtime", true);

    std::unordered_map<std::string, std::unordered_set<std::string>> points;
    parse_devices(root, config, points);
    parse_groups(root, config, points);
    config.processors = parse_plugins(root, "processors");
    config.event_publishers = parse_plugins(root, "event_publishers");
    return config;
}

}  // namespace

ApplicationConfig parse_config(
    std::string_view json_text,
    std::string_view source_name) {
    const auto source = source_name.empty()
        ? std::string{"<memory>"}
        : std::string{source_name};
    try {
        const auto root = Json::parse(json_text.begin(), json_text.end());
        return parse_application_config(root);
    } catch (const ConfigError& error) {
        throw std::runtime_error{
            "invalid configuration in " + source + ": " + error.what()};
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error{
            "invalid JSON in " + source + ": " + error.what()};
    }
}

ApplicationConfig load_config(const std::filesystem::path& path) {
    std::ifstream input{path, std::ios::binary};
    if (!input.is_open()) {
        throw std::runtime_error{
            "cannot open configuration file: " + path.string()};
    }

    const std::string contents{
        std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (input.bad()) {
        throw std::runtime_error{
            "cannot read configuration file: " + path.string()};
    }
    return parse_config(contents, path.string());
}

}  // namespace gateway
