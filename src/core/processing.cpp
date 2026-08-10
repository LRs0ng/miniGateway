#include "gateway/processing.hpp"

#include <stdexcept>
#include <utility>

namespace gateway {

ProcessingPipeline::ProcessingPipeline(
    std::vector<std::unique_ptr<IDataProcessor>> processors,
    ControlSink control)
    : processors_(std::move(processors)),
      control_(std::move(control)) {
    for (const auto& processor : processors_) {
        if (!processor) {
            throw std::invalid_argument("processing pipeline contains a null processor");
        }
    }
}

void ProcessingPipeline::set_control_sink(ControlSink control) {
    control_ = std::move(control);
}

void ProcessingPipeline::process(Event& event) {
    ProcessingContext context{
        .now_ns = unix_time_ns(),
        .control = control_,
    };
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

}  // namespace gateway
