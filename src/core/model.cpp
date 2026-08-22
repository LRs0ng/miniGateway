#include "gateway/model.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>
#include <type_traits>

namespace gateway {

std::int64_t unix_time_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::optional<double> numeric_value(const Scalar& value) {
    return std::visit(
        [](const auto& item) -> std::optional<double> {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                return static_cast<double>(item);
            } else if constexpr (std::is_same_v<T, double>) {
                return item;
            } else if constexpr (std::is_same_v<T, bool>) {
                return item ? 1.0 : 0.0;
            } else if constexpr (std::is_same_v<T, ByteArray>) {
                return std::nullopt;
            } else {
                double result{};
                const auto* begin = item.data();
                const auto* end = begin + item.size();
                const auto parsed = std::from_chars(begin, end, result);
                if (parsed.ec == std::errc{} && parsed.ptr == end) {
                    return result;
                }
                return std::nullopt;
            }
        },
        value);
}

std::string scalar_to_string(const Scalar& value) {
    return std::visit(
        [](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, bool>) {
                return std::string{item ? "true" : "false"};
            } else if constexpr (std::is_same_v<T, double>) {
                std::ostringstream stream;
                stream << std::fixed << std::setprecision(3) << item;
                return stream.str();
            } else if constexpr (std::is_same_v<T, std::int64_t>) {
                return std::to_string(item);
            } else if constexpr (std::is_same_v<T, ByteArray>) {
                return "<" + std::to_string(item.size()) + " bytes>";
            } else {
                return item;
            }
        },
        value);
}

std::string_view quality_name(Quality quality) {
    switch (quality) {
        case Quality::Good:
            return "good";
        case Quality::Timeout:
            return "timeout";
        case Quality::Disconnected:
            return "disconnected";
        case Quality::DecodeError:
            return "decode_error";
        case Quality::OutOfRange:
            return "out_of_range";
        case Quality::Bad:
            return "bad";
    }
    return "unknown";
}

const Reading* find_reading(const Event& event, std::string_view point) {
    for (const auto& reading : event.readings) {
        if (reading.point == point) {
            return &reading;
        }
    }
    return nullptr;
}

Reading* find_reading(Event& event, std::string_view point) {
    for (auto& reading : event.readings) {
        if (reading.point == point) {
            return &reading;
        }
    }
    return nullptr;
}

}  // namespace gateway
