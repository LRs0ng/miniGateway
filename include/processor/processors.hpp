#pragma once

#include "gateway/plugin_registry.hpp"

#include <cstddef>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

namespace gateway {

class ThresholdProcessor final : public IDataProcessor {
public:
    ThresholdProcessor(
        std::string input,
        double greater_than,
        std::string output);

    void process(Event& event, ProcessingContext& context) override;

private:
    std::string input_;
    double greater_than_;
    std::string output_;
};

class WindowAverageProcessor final : public IDataProcessor {
public:
    WindowAverageProcessor(
        std::string input,
        std::size_t window,
        std::string output);

    void process(Event& event, ProcessingContext& context) override;

private:
    std::string input_;
    std::size_t window_;
    std::string output_;
    std::unordered_map<std::string, std::deque<double>> windows_;
};

class InferenceProcessor final : public IDataProcessor {
public:
    InferenceProcessor(
        std::string runner,
        std::vector<std::string> inputs,
        std::vector<std::string> outputs);

    void process(Event& event, ProcessingContext& context) override;

private:
    std::string runner_;
    std::vector<std::string> inputs_;
    std::vector<std::string> outputs_;
};

void register_threshold_processor_plugin(PluginRegistry& registry);
void register_window_average_processor_plugin(PluginRegistry& registry);
void register_inference_processor_plugin(PluginRegistry& registry);

}  // namespace gateway
