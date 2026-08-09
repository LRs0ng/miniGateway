#pragma once

#include "gateway/acquisition.hpp"
#include "gateway/config.hpp"
#include "gateway/event_publisher.hpp"
#include "gateway/processing.hpp"

#include <memory>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace gateway {

// std::function keeps the simple function-pointer registration API source
// compatible while allowing startup-loaded factories to capture their shared
// library handle and C destroy function.
using DriverFactory =
    std::function<std::unique_ptr<IProtocolDriver>(std::string_view)>;
using ProcessorFactory =
    std::function<std::unique_ptr<IDataProcessor>(std::string_view)>;
using EventPublisherFactory =
    std::function<std::unique_ptr<IEventPublisher>(std::string_view)>;

// Load all libraries named by the configuration. This operation is intended
// to be called once before PluginRegistry::create(); no library is loaded by
// the runtime after it has started.
class DynamicPluginLoader;

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

    // Load all libraries named by the configuration once during startup.
    // `PluginSpec::library` is passed to the platform loader unchanged. A
    // path containing `./` or another directory separator is interpreted
    // from the process working directory; a bare filename follows the
    // platform loader's search rules. In both cases the filename must include
    // its platform-specific suffix.
    void load_dynamic_plugins(const ApplicationConfig& config);

    [[nodiscard]] PluginInstances create(
        const ApplicationConfig& config) const;

private:
    std::unordered_map<std::string, DriverFactory> driver_factories_;
    std::unordered_map<std::string, ProcessorFactory> processor_factories_;
    std::unordered_map<std::string, EventPublisherFactory>
        event_publisher_factories_;
    std::vector<std::shared_ptr<DynamicPluginLoader>> dynamic_plugins_;
    std::unordered_map<std::string, std::filesystem::path>
        dynamic_plugin_sources_;
    std::unordered_map<std::string, std::string> dynamic_library_owners_;
    bool dynamic_loading_called_{false};
};

}  // namespace gateway
