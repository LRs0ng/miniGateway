#pragma once

#include "gateway/control.hpp"
#include "gateway/model.hpp"

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace gateway {

struct ProcessingContext {
    std::int64_t now_ns{0};
    ControlSink control{};

    // Submission is deliberately non-blocking. A processor can inspect the
    // return value for backpressure, but it never waits for device I/O here.
    [[nodiscard]] ControlSubmitResult submit_control(
        DeviceControlRequest request) {
        if (!control) {
            return ControlSubmitResult::Stopping;
        }
        return control(std::move(request), {});
    }
};

class IDataProcessor {
public:
    virtual ~IDataProcessor() = default;
    virtual void process(Event& event, ProcessingContext& context) = 0;
};

struct ProcessingStats {
    std::uint64_t events{0};
    std::uint64_t processor_errors{0};
};

class ProcessingPipeline {
public:
    explicit ProcessingPipeline(
        std::vector<std::unique_ptr<IDataProcessor>> processors,
        ControlSink control = {});

    // Runtime binds the sink after all drivers and their gates have been
    // validated. This keeps processor construction independent of Runtime.
    void set_control_sink(ControlSink control);

    void process(Event& event);
    [[nodiscard]] ProcessingStats stats() const;

private:
    std::vector<std::unique_ptr<IDataProcessor>> processors_;
    ControlSink control_;
    std::atomic<std::uint64_t> events_{0};
    std::atomic<std::uint64_t> processor_errors_{0};
};

}  // namespace gateway
