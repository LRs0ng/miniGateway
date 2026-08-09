#include "print_event_publisher.hpp"

#include "plugin_support/plugin_json.hpp"
#include "gateway/plugin_api.hpp"

#include <iostream>
#include <stdexcept>

namespace gateway {

namespace {

std::unique_ptr<PrintEventPublisher> make_print_publisher(
    std::string_view settings_json) {
    constexpr std::string_view plugin{"print event publisher"};
    const auto settings =
        plugin_json::parse_object(settings_json, plugin);
    return std::make_unique<PrintEventPublisher>(
        plugin_json::bool_member(settings, "include_readings", plugin));
}

}  // namespace

PrintEventPublisher::PrintEventPublisher(bool include_readings)
    : include_readings_(include_readings) {}

void PrintEventPublisher::configure(const GatewayConfig&) {
    if (started_) {
        throw std::logic_error("cannot configure a running print publisher");
    }
    configured_ = true;
}

void PrintEventPublisher::start() {
    if (!configured_) {
        throw std::logic_error("print publisher is not configured");
    }
    started_ = true;
}

EventPublishResult PrintEventPublisher::publish(const Event& event) {
    if (!started_) {
        return EventPublishResult::Unavailable;
    }

    std::cout << event.event_id << " device=" << event.device_id
              << " source=" << event.source;
    if (!event.model_version.empty()) {
        std::cout << " model=" << event.model_version;
    }
    std::cout << '\n';

    if (include_readings_) {
        for (const auto& reading : event.readings) {
            std::cout << "  " << (reading.derived ? "[derived] " : "")
                      << reading.point << '=' << scalar_to_string(reading.value);
            if (!reading.unit.empty()) {
                std::cout << ' ' << reading.unit;
            }
            std::cout << " quality=" << quality_name(reading.quality) << '\n';
        }
    }
    ++published_;
    return EventPublishResult::Accepted;
}

void PrintEventPublisher::stop() noexcept {
    started_ = false;
}

std::uint64_t PrintEventPublisher::published() const noexcept {
    return published_;
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return make_print_publisher(
                   settings_json == nullptr ? std::string_view{"{}"}
                                             : std::string_view{settings_json})
            .release();
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<PrintEventPublisher*>(plugin);
}

}  // namespace gateway
