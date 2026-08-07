#pragma once

#include "gateway/model.hpp"

#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gateway {

struct ProcessingContext {
    std::int64_t now_ns{0};
};

class IDataProcessor {
public:
    virtual ~IDataProcessor() = default;
    virtual std::string_view name() const noexcept = 0;
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

class ThresholdProcessor final : public IDataProcessor {
public:
    ThresholdProcessor(std::string input, double greater_than, std::string output);

    std::string_view name() const noexcept override;
    void process(Event& event, ProcessingContext& context) override;

private:
    std::string input_;
    double greater_than_;
    std::string output_;
};

class WindowAverageProcessor final : public IDataProcessor {
public:
    WindowAverageProcessor(std::string input, std::size_t window, std::string output);

    std::string_view name() const noexcept override;
    void process(Event& event, ProcessingContext& context) override;

private:
    std::string input_;
    std::size_t window_;
    std::string output_;
    std::unordered_map<std::string, std::deque<double>> windows_;
};

class IModelRunner {
public:
    virtual ~IModelRunner() = default;
    virtual std::string_view version() const noexcept = 0;
    virtual std::vector<double> run(std::span<const double> inputs) = 0;
};

class InferenceProcessor final : public IDataProcessor {
public:
    InferenceProcessor(
        std::vector<std::string> inputs,
        std::vector<std::string> outputs,
        std::shared_ptr<IModelRunner> runner);

    std::string_view name() const noexcept override;
    void process(Event& event, ProcessingContext& context) override;

private:
    std::vector<std::string> inputs_;
    std::vector<std::string> outputs_;
    std::shared_ptr<IModelRunner> runner_;
};

}  // namespace gateway
