#pragma once

#include "gateway/acquisition.hpp"
#include "gateway/config.hpp"
#include "gateway/event_publisher.hpp"
#include "gateway/processing.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gateway {

using DriverFactory =
    std::unique_ptr<IProtocolDriver> (*)(std::string_view);
using ProcessorFactory =
    std::unique_ptr<IDataProcessor> (*)(std::string_view);
using EventPublisherFactory =
    std::unique_ptr<IEventPublisher> (*)(std::string_view);

struct PluginInstances {
    std::vector<DriverInstance> drivers;
    std::vector<std::unique_ptr<IDataProcessor>> processors;
    std::vector<EventPublisherInstance> event_publishers;
};

class PluginRegistry {
public:
    void register_driver(std::string type, DriverFactory factory);
    void register_processor(std::string type, ProcessorFactory factory);
    void register_event_publisher(
        std::string type,
        EventPublisherFactory factory);

    [[nodiscard]] PluginInstances create(
        const ApplicationConfig& config) const;

private:
    std::unordered_map<std::string, DriverFactory> driver_factories_;
    std::unordered_map<std::string, ProcessorFactory> processor_factories_;
    std::unordered_map<std::string, EventPublisherFactory>
        event_publisher_factories_;
};

}  // namespace gateway
