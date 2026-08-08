#include "processor/processors.hpp"

#include "plugin_support/plugin_json.hpp"
#include "processor_support.hpp"

#include <numeric>
#include <stdexcept>
#include <utility>

namespace gateway {

WindowAverageProcessor::WindowAverageProcessor(
    std::string input,
    std::size_t window,
    std::string output)
    : input_(std::move(input)), window_(window), output_(std::move(output)) {
    if (input_.empty() || output_.empty() || window_ == 0) {
        throw std::invalid_argument(
            "window rule requires input, output and positive size");
    }
}

void WindowAverageProcessor::process(Event& event, ProcessingContext&) {
    const auto* input = find_reading(event, input_);
    if (input == nullptr || input->quality != Quality::Good) {
        return;
    }
    const auto value = numeric_value(input->value);
    if (!value) {
        return;
    }

    auto& values = windows_[event.device_id];
    values.push_back(*value);
    while (values.size() > window_) {
        values.pop_front();
    }
    if (values.size() != window_) {
        return;
    }

    const auto sum = std::accumulate(values.begin(), values.end(), 0.0);
    event.readings.push_back(processor_support::derived_reading(
        output_, sum / static_cast<double>(values.size()), input->source_time_ns,
        input->received_time_ns));
}

void register_window_average_processor_plugin(PluginRegistry& registry) {
    registry.register_processor(
        "window_average",
        [](std::string_view settings_json)
            -> std::unique_ptr<IDataProcessor> {
            constexpr std::string_view plugin{"window average processor"};
            const auto settings =
                plugin_json::parse_object(settings_json, plugin);
            return std::make_unique<WindowAverageProcessor>(
                plugin_json::string_member(settings, "input", plugin),
                plugin_json::size_member(settings, "window", plugin, true),
                plugin_json::string_member(settings, "output", plugin));
        });
}

}  // namespace gateway
