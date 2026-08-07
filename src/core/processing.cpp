#include "gateway/processing.hpp"

#include <numeric>
#include <stdexcept>
#include <utility>

namespace gateway {
namespace {

Reading derived_reading(
    std::string point,
    Scalar value,
    std::int64_t source_time_ns,
    std::int64_t received_time_ns) {
    return Reading{
        .point = std::move(point),
        .value = std::move(value),
        .quality = Quality::Good,
        .unit = {},
        .source_time_ns = source_time_ns,
        .received_time_ns = received_time_ns,
        .derived = true,
    };
}

}  // namespace

ProcessingPipeline::ProcessingPipeline(
    std::vector<std::unique_ptr<IDataProcessor>> processors)
    : processors_(std::move(processors)) {
    for (const auto& processor : processors_) {
        if (!processor) {
            throw std::invalid_argument("processing pipeline contains a null processor");
        }
    }
}

void ProcessingPipeline::process(Event& event) {
    ProcessingContext context{.now_ns = unix_time_ns()};
    for (auto& processor : processors_) {
        try {
            processor->process(event, context);
        } catch (...) {
            processor_errors_.fetch_add(1, std::memory_order_relaxed);
        }
    }
    events_.fetch_add(1, std::memory_order_relaxed);
}

ProcessingStats ProcessingPipeline::stats() const {
    return ProcessingStats{
        .events = events_.load(std::memory_order_relaxed),
        .processor_errors = processor_errors_.load(std::memory_order_relaxed),
    };
}

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

std::string_view ThresholdProcessor::name() const noexcept {
    return "threshold";
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
    event.readings.push_back(derived_reading(
        output_, *value > greater_than_, input->source_time_ns,
        input->received_time_ns));
}

WindowAverageProcessor::WindowAverageProcessor(
    std::string input,
    std::size_t window,
    std::string output)
    : input_(std::move(input)), window_(window), output_(std::move(output)) {
    if (input_.empty() || output_.empty() || window_ == 0) {
        throw std::invalid_argument("window rule requires input, output and positive size");
    }
}

std::string_view WindowAverageProcessor::name() const noexcept {
    return "window_average";
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
    event.readings.push_back(derived_reading(
        output_, sum / static_cast<double>(values.size()), input->source_time_ns,
        input->received_time_ns));
}

InferenceProcessor::InferenceProcessor(
    std::vector<std::string> inputs,
    std::vector<std::string> outputs,
    std::shared_ptr<IModelRunner> runner)
    : inputs_(std::move(inputs)),
      outputs_(std::move(outputs)),
      runner_(std::move(runner)) {
    if (inputs_.empty() || outputs_.empty() || !runner_) {
        throw std::invalid_argument("inference inputs, outputs and runner are required");
    }
}

std::string_view InferenceProcessor::name() const noexcept {
    return "inference";
}

void InferenceProcessor::process(Event& event, ProcessingContext& context) {
    std::vector<double> inputs;
    inputs.reserve(inputs_.size());
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
        inputs.push_back(*value);
        source_time = reading->source_time_ns;
        received_time = reading->received_time_ns;
    }

    auto outputs = runner_->run(inputs);
    if (outputs.size() != outputs_.size()) {
        throw std::runtime_error("model output count does not match configuration");
    }
    for (std::size_t index = 0; index < outputs.size(); ++index) {
        event.readings.push_back(derived_reading(
            outputs_[index], outputs[index], source_time, received_time));
    }
    event.model_version = std::string{runner_->version()};
}

}  // namespace gateway
