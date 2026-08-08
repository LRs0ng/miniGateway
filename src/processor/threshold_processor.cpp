#include "processor/processors.hpp"

#include "plugin_support/plugin_json.hpp"
#include "processor_support.hpp"

#include <stdexcept>
#include <utility>

namespace gateway {

ThresholdProcessor::ThresholdProcessor(
    std::string input,
    double greater_than,
    std::string output)
    : input_(std::move(input)),
      greater_than_(greater_than),
      output_(std::move(output)) {
    if (input_.empty() || output_.empty()) {
        throw std::invalid_argument("threshold input and output are required");
    }
}

void ThresholdProcessor::process(Event& event, ProcessingContext&) {
    const auto* input = find_reading(event, input_);
    if (input == nullptr || input->quality != Quality::Good) {
        return;
    }
    const auto value = numeric_value(input->value);
    if (!value) {
        return;
    }
    event.readings.push_back(processor_support::derived_reading(
        output_, *value > greater_than_, input->source_time_ns,
        input->received_time_ns));
}

void register_threshold_processor_plugin(PluginRegistry& registry) {
    registry.register_processor(
        "threshold",
        [](std::string_view settings_json)
            -> std::unique_ptr<IDataProcessor> {
            constexpr std::string_view plugin{"threshold processor"};
            const auto settings =
                plugin_json::parse_object(settings_json, plugin);
            return std::make_unique<ThresholdProcessor>(
                plugin_json::string_member(settings, "input", plugin),
                plugin_json::number_member(settings, "greater_than", plugin),
                plugin_json::string_member(settings, "output", plugin));
        });
}

}  // namespace gateway
