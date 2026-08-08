#pragma once

#include "gateway/model.hpp"

#include <atomic>
#include <memory>
#include <vector>

namespace gateway {

struct ProcessingContext {
    std::int64_t now_ns{0};
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
    explicit ProcessingPipeline(std::vector<std::unique_ptr<IDataProcessor>> processors);

    void process(Event& event);
    [[nodiscard]] ProcessingStats stats() const;

private:
    std::vector<std::unique_ptr<IDataProcessor>> processors_;
    std::atomic<std::uint64_t> events_{0};
    std::atomic<std::uint64_t> processor_errors_{0};
};

}  // namespace gateway
