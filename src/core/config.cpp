#include "gateway/config.hpp"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
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

bool bool_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    const auto& value = member(object, key, path);
    if (!value.is_boolean()) {
        fail(std::string{path} + "." + std::string{key}, "must be a boolean");
    }
    return value.get<bool>();
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

int int_member_in_range(
    const Json& object,
    std::string_view key,
    std::string_view path,
    int minimum,
    int maximum) {
    const auto value = unsigned_integer_member(object, key, path);
    if (value < static_cast<std::uint64_t>(minimum) ||
        value > static_cast<std::uint64_t>(maximum)) {
        fail(
            std::string{path} + "." + std::string{key},
            "must be in range " + std::to_string(minimum) + ".." +
                std::to_string(maximum));
    }
    return static_cast<int>(value);
}

unsigned unsigned_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    const auto value = unsigned_integer_member(object, key, path);
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<unsigned>::max())) {
        fail(std::string{path} + "." + std::string{key}, "is too large");
    }
    return static_cast<unsigned>(value);
}

double finite_number_value(const Json& value, std::string_view path) {
    if (!value.is_number()) {
        fail(path, "must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        fail(path, "must be finite");
    }
    return result;
}

double finite_number_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    return finite_number_value(
        member(object, key, path),
        std::string{path} + "." + std::string{key});
}

std::optional<double> optional_number_member(
    const Json& object,
    std::string_view key,
    std::string_view path) {
    const auto item = object.find(key);
    if (item == object.end() || item->is_null()) {
        return std::nullopt;
    }
    return finite_number_value(
        *item, std::string{path} + "." + std::string{key});
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
    if (!value.is_array()) {
        fail(path, "must be an array");
    }
    if (require_non_empty && value.empty()) {
        fail(path, "must not be empty");
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
    std::unordered_map<std::string, std::unordered_set<std::string>>& points,
    std::unordered_map<std::string, std::string>& driver_types) {
    const auto& devices = array_member(root, "devices", "root");
    if (devices.empty()) {
        fail("root.devices", "must not be empty");
    }

    std::unordered_set<std::string> device_ids;
    config.gateway.devices.reserve(devices.size());
    config.drivers.reserve(devices.size());
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
        device.driver = string_member(value, "driver", path);
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

        const auto& driver_config = object_member(value, "driver_config", path);
        DriverConfig driver;
        driver.device_id = device.id;
        if (device.driver == "simulator_poll") {
            driver.settings = PollSimulatorConfig{
                .latency = milliseconds_member(
                    driver_config, "latency_ms", path + ".driver_config", false),
            };
        } else if (device.driver == "simulator_push") {
            driver.settings = PushSimulatorConfig{
                .interval = milliseconds_member(
                    driver_config, "interval_ms", path + ".driver_config", true),
            };
        } else {
            fail(path + ".driver", "unsupported driver '" + device.driver + "'");
        }

        driver_types.emplace(device.id, device.driver);
        config.gateway.devices.push_back(std::move(device));
        config.drivers.push_back(std::move(driver));
    }
}

void parse_groups(
    const Json& root,
    ApplicationConfig& config,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& points,
    const std::unordered_map<std::string, std::string>& driver_types) {
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
        if (driver_types.at(group.device_id) != "simulator_poll") {
            fail(path + ".device_id", "push device cannot own a collection group");
        }
        for (const auto& point : group.points) {
            if (!device_points->second.contains(point)) {
                fail(path + ".points", "references unknown point '" + point + "'");
            }
        }
        config.gateway.groups.push_back(std::move(group));
    }
}

void parse_processors(
    const Json& root,
    ApplicationConfig& config,
    const std::unordered_map<std::string, std::unordered_set<std::string>>& points) {
    const auto& processors = array_member(root, "processors", "root");
    std::unordered_set<std::string> available_points;
    for (const auto& [device_id, device_points] : points) {
        static_cast<void>(device_id);
        available_points.insert(device_points.begin(), device_points.end());
    }

    auto require_input = [&available_points](
                             const std::string& input,
                             const std::string& path) {
        if (!available_points.contains(input)) {
            fail(path, "references unknown input point '" + input + "'");
        }
    };
    auto add_output = [&available_points](
                          const std::string& output,
                          const std::string& path) {
        if (!available_points.insert(output).second) {
            fail(path, "duplicates an existing or derived point '" + output + "'");
        }
    };

    config.processors.reserve(processors.size());
    for (std::size_t index = 0; index < processors.size(); ++index) {
        const auto path = "root.processors[" + std::to_string(index) + "]";
        const auto& value = processors[index];
        if (!value.is_object()) {
            fail(path, "must be an object");
        }
        const auto type = string_member(value, "type", path);
        if (type == "threshold") {
            auto threshold = ThresholdProcessorConfig{
                .input = string_member(value, "input", path),
                .greater_than = finite_number_member(value, "greater_than", path),
                .output = string_member(value, "output", path),
            };
            require_input(threshold.input, path + ".input");
            add_output(threshold.output, path + ".output");
            config.processors.push_back(std::move(threshold));
        } else if (type == "window_average") {
            auto average = WindowAverageProcessorConfig{
                .input = string_member(value, "input", path),
                .window = size_member(value, "window", path, true),
                .output = string_member(value, "output", path),
            };
            require_input(average.input, path + ".input");
            add_output(average.output, path + ".output");
            config.processors.push_back(std::move(average));
        } else if (type == "inference") {
            auto inference = InferenceProcessorConfig{
                .runner = string_member(value, "runner", path),
                .inputs = string_array(
                    member(value, "inputs", path), path + ".inputs", true),
                .outputs = string_array(
                    member(value, "outputs", path), path + ".outputs", true),
            };
            if (inference.runner != "demo_rms") {
                fail(path + ".runner", "unsupported model runner '" +
                    inference.runner + "'");
            }
            if (inference.outputs.size() != 1) {
                fail(path + ".outputs", "demo_rms requires exactly one output");
            }
            for (std::size_t input_index = 0;
                 input_index < inference.inputs.size();
                 ++input_index) {
                require_input(
                    inference.inputs[input_index],
                    path + ".inputs[" + std::to_string(input_index) + "]");
            }
            for (std::size_t output_index = 0;
                 output_index < inference.outputs.size();
                 ++output_index) {
                add_output(
                    inference.outputs[output_index],
                    path + ".outputs[" + std::to_string(output_index) + "]");
            }
            config.processors.push_back(std::move(inference));
        } else {
            fail(path + ".type", "unsupported processor '" + type + "'");
        }
    }
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
    std::unordered_map<std::string, std::string> driver_types;
    parse_devices(root, config, points, driver_types);
    parse_groups(root, config, points, driver_types);
    parse_processors(root, config, points);
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
