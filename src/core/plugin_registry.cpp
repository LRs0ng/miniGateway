#include "gateway/plugin_registry.hpp"

#include <stdexcept>
#include <utility>

namespace gateway {
namespace {

template <typename Factory>
void register_factory(
    std::unordered_map<std::string, Factory>& factories,
    std::string type,
    Factory factory,
    std::string_view category) {
    if (type.empty() || !factory) {
        throw std::invalid_argument(
            std::string{category} + " plugin type and factory are required");
    }
    if (!factories.emplace(type, std::move(factory)).second) {
        throw std::invalid_argument(
            "duplicate " + std::string{category} + " plugin type: " + type);
    }
}

template <typename FactoryMap>
const typename FactoryMap::mapped_type& find_factory(
    const FactoryMap& factories,
    const std::string& type,
    std::string_view category) {
    const auto factory = factories.find(type);
    if (factory == factories.end()) {
        throw std::invalid_argument(
            "unknown " + std::string{category} + " plugin type: " + type);
    }
    return factory->second;
}

}  // namespace

void PluginRegistry::register_driver(std::string type, DriverFactory factory) {
    register_factory(
        driver_factories_, std::move(type), std::move(factory), "driver");
}

void PluginRegistry::register_processor(
    std::string type,
    ProcessorFactory factory) {
    register_factory(
        processor_factories_, std::move(type), std::move(factory), "processor");
}

void PluginRegistry::register_event_publisher(
    std::string type,
    EventPublisherFactory factory) {
    register_factory(
        event_publisher_factories_,
        std::move(type),
        std::move(factory),
        "event publisher");
}

PluginInstances PluginRegistry::create(const ApplicationConfig& config) const {
    PluginInstances instances;
    instances.drivers.reserve(config.gateway.devices.size());
    for (const auto& device : config.gateway.devices) {
        const auto& spec = device.driver;
        const auto& factory = find_factory(
            driver_factories_, spec.type, "driver");
        auto driver = factory(spec.settings_json);
        if (!driver) {
            throw std::runtime_error(
                "driver plugin factory returned null: " + spec.type);
        }
        instances.drivers.push_back(DriverInstance{
            .device_id = device.id,
            .driver = std::move(driver),
        });
    }

    instances.processors.reserve(config.processors.size());
    for (const auto& spec : config.processors) {
        if (!spec.enabled) {
            continue;
        }
        const auto& factory = find_factory(
            processor_factories_, spec.type, "processor");
        auto processor = factory(spec.settings_json);
        if (!processor) {
            throw std::runtime_error(
                "processor plugin factory returned null: " + spec.type);
        }
        instances.processors.push_back(std::move(processor));
    }

    instances.event_publishers.reserve(config.event_publishers.size());
    for (const auto& spec : config.event_publishers) {
        if (!spec.enabled) {
            continue;
        }
        const auto& factory = find_factory(
            event_publisher_factories_, spec.type, "event publisher");
        auto publisher = factory(spec.settings_json);
        if (!publisher) {
            throw std::runtime_error(
                "event publisher plugin factory returned null: " + spec.type);
        }
        instances.event_publishers.push_back(EventPublisherInstance{
            .id = spec.id,
            .publisher = std::move(publisher),
        });
    }
    return instances;
}

}  // namespace gateway
