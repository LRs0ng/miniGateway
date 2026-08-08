#pragma once

#include <nlohmann/json.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gateway::plugin_json {

using Json = nlohmann::json;

[[noreturn]] inline void fail(
    std::string_view plugin,
    std::string_view field,
    std::string_view message) {
    std::string text{"invalid "};
    text += plugin;
    text += " configuration";
    if (!field.empty()) {
        text += ".";
        text += field;
    }
    text += ": ";
    text += message;
    throw std::invalid_argument{std::move(text)};
}

inline Json parse_object(
    std::string_view settings_json,
    std::string_view plugin) {
    try {
        auto value = Json::parse(settings_json.begin(), settings_json.end());
        if (!value.is_object()) {
            fail(plugin, {}, "must be an object");
        }
        return value;
    } catch (const nlohmann::json::exception& error) {
        fail(plugin, {}, std::string{"contains invalid JSON: "} + error.what());
    }
}

inline const Json& member(
    const Json& object,
    std::string_view key,
    std::string_view plugin) {
    const auto item = object.find(key);
    if (item == object.end()) {
        fail(plugin, key, "is required");
    }
    return *item;
}

inline std::string string_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin,
    bool allow_empty = false) {
    const auto& value = member(object, key, plugin);
    if (!value.is_string()) {
        fail(plugin, key, "must be a string");
    }
    auto result = value.get<std::string>();
    if (!allow_empty && result.empty()) {
        fail(plugin, key, "must not be empty");
    }
    return result;
}

inline bool bool_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin) {
    const auto& value = member(object, key, plugin);
    if (!value.is_boolean()) {
        fail(plugin, key, "must be a boolean");
    }
    return value.get<bool>();
}

inline std::uint64_t unsigned_integer_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin) {
    const auto& value = member(object, key, plugin);
    if (!value.is_number_integer()) {
        fail(plugin, key, "must be a non-negative integer");
    }
    if (value.is_number_unsigned()) {
        return value.get<std::uint64_t>();
    }
    const auto result = value.get<std::int64_t>();
    if (result < 0) {
        fail(plugin, key, "must be a non-negative integer");
    }
    return static_cast<std::uint64_t>(result);
}

inline std::size_t size_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin,
    bool positive = false) {
    const auto value = unsigned_integer_member(object, key, plugin);
    if ((positive && value == 0) ||
        value > static_cast<std::uint64_t>(
                    std::numeric_limits<std::size_t>::max())) {
        fail(plugin, key, positive ? "must be positive" : "is too large");
    }
    return static_cast<std::size_t>(value);
}

inline std::chrono::milliseconds milliseconds_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin,
    bool positive = false) {
    using Rep = std::chrono::milliseconds::rep;
    const auto value = unsigned_integer_member(object, key, plugin);
    if ((positive && value == 0) ||
        value > static_cast<std::uint64_t>(std::numeric_limits<Rep>::max())) {
        fail(plugin, key, positive ? "must be positive" : "is too large");
    }
    return std::chrono::milliseconds{static_cast<Rep>(value)};
}

inline int int_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin,
    int minimum,
    int maximum) {
    const auto value = unsigned_integer_member(object, key, plugin);
    if (value < static_cast<std::uint64_t>(minimum) ||
        value > static_cast<std::uint64_t>(maximum)) {
        fail(
            plugin,
            key,
            "must be in range " + std::to_string(minimum) + ".." +
                std::to_string(maximum));
    }
    return static_cast<int>(value);
}

inline unsigned unsigned_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin) {
    const auto value = unsigned_integer_member(object, key, plugin);
    if (value > static_cast<std::uint64_t>(
                    std::numeric_limits<unsigned>::max())) {
        fail(plugin, key, "is too large");
    }
    return static_cast<unsigned>(value);
}

inline double number_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin) {
    const auto& value = member(object, key, plugin);
    if (!value.is_number()) {
        fail(plugin, key, "must be a number");
    }
    const auto result = value.get<double>();
    if (!std::isfinite(result)) {
        fail(plugin, key, "must be finite");
    }
    return result;
}

inline std::vector<std::string> string_array_member(
    const Json& object,
    std::string_view key,
    std::string_view plugin,
    bool require_non_empty = false) {
    const auto& value = member(object, key, plugin);
    if (!value.is_array() || (require_non_empty && value.empty())) {
        fail(
            plugin,
            key,
            require_non_empty ? "must be a non-empty array" : "must be an array");
    }

    std::vector<std::string> result;
    std::unordered_set<std::string> unique;
    result.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (!value[index].is_string()) {
            fail(plugin, key, "must contain only strings");
        }
        auto item = value[index].get<std::string>();
        if (item.empty() || !unique.insert(item).second) {
            fail(plugin, key, "must contain unique non-empty strings");
        }
        result.push_back(std::move(item));
    }
    return result;
}

}  // namespace gateway::plugin_json
