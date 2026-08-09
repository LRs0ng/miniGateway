#include "processor/processors.hpp"

#include "plugin_support/plugin_json.hpp"
#include "gateway/plugin_api.hpp"
#include "processor_support.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace gateway {

namespace {

std::unique_ptr<InferenceProcessor> make_inference_processor(
    std::string_view settings_json) {
    constexpr std::string_view plugin{"inference processor"};
    const auto settings =
        plugin_json::parse_object(settings_json, plugin);
    return std::make_unique<InferenceProcessor>(
        plugin_json::string_member(settings, "runner", plugin),
        plugin_json::string_array_member(settings, "inputs", plugin, true),
        plugin_json::string_array_member(settings, "outputs", plugin, true));
}

}  // namespace

InferenceProcessor::InferenceProcessor(
    std::string runner,
    std::vector<std::string> inputs,
    std::vector<std::string> outputs)
    : runner_(std::move(runner)),
      inputs_(std::move(inputs)),
      outputs_(std::move(outputs)) {
    if (runner_ != "demo_rms") {
        throw std::invalid_argument("unsupported inference runner: " + runner_);
    }
    if (inputs_.empty() || outputs_.size() != 1) {
        throw std::invalid_argument(
            "demo_rms requires at least one input and exactly one output");
    }
}

void InferenceProcessor::process(Event& event, ProcessingContext& context) {
    double squared_sum = 0.0;
    std::int64_t source_time = context.now_ns;
    std::int64_t received_time = context.now_ns;

    for (const auto& point : inputs_) {
        const auto* reading = find_reading(event, point);
        if (reading == nullptr || reading->quality != Quality::Good) {
            return;
        }
        const auto value = numeric_value(reading->value);
        if (!value) {
            return;
        }
        squared_sum += *value * *value;
        source_time = reading->source_time_ns;
        received_time = reading->received_time_ns;
    }

    const auto result = std::sqrt(
        squared_sum / static_cast<double>(inputs_.size()));
    event.readings.push_back(processor_support::derived_reading(
        outputs_.front(), result, source_time, received_time));
    event.model_version = "demo-rms-v1";
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return make_inference_processor(
                   settings_json == nullptr ? std::string_view{"{}"}
                                             : std::string_view{settings_json})
            .release();
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<InferenceProcessor*>(plugin);
}

}  // namespace gateway
