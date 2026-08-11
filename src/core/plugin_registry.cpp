#include "gateway/plugin_registry.hpp"

#include "gateway/plugin_api.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <stdexcept>
#include <string>
#include <utility>

namespace gateway {
namespace {

enum class PluginKind {
    Driver,
    Processor,
    EventPublisher,
    DeviceControlSource,
};

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

std::string path_text(const std::filesystem::path& path) {
    return path.string();
}

void validate_library_filename(
    const std::filesystem::path& configured,
    std::string_view type) {
    if (configured.empty()) {
        throw std::invalid_argument(
            "plugin '" + std::string{type} +
            "' must specify a library path");
    }
    if (configured.filename().empty() || !configured.has_extension()) {
        throw std::invalid_argument(
            "plugin '" + std::string{type} +
            "' library must be a complete filename with a suffix: " +
            path_text(configured));
    }
}

}  // namespace

// Owns one operating-system module handle. A shared_ptr to this object is
// captured by each factory and adapter, so a plugin cannot be unloaded while
// an instance (or a factory that may create one) still exists.
class DynamicPluginLoader {
public:
    explicit DynamicPluginLoader(std::filesystem::path path)
        : path_(std::move(path)) {
#if defined(_WIN32)
        handle_ = ::LoadLibraryW(path_.wstring().c_str());
        if (handle_ == nullptr) {
            throw std::runtime_error(
                "LoadLibrary failed for " + path_text(path_));
        }
#else
        handle_ = ::dlopen(path_.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle_ == nullptr) {
            const auto* detail = ::dlerror();
            throw std::runtime_error(
                "dlopen failed for " + path_text(path_) +
                (detail == nullptr ? std::string{} : std::string{": "} + detail));
        }
#endif
    }

    ~DynamicPluginLoader() {
#if defined(_WIN32)
        if (handle_ != nullptr) {
            (void)::FreeLibrary(handle_);
        }
#else
        if (handle_ != nullptr) {
            (void)::dlclose(handle_);
        }
#endif
    }

    DynamicPluginLoader(const DynamicPluginLoader&) = delete;
    DynamicPluginLoader& operator=(const DynamicPluginLoader&) = delete;

    [[nodiscard]] void* symbol(std::string_view name) const {
        std::string symbol_name{name};
#if defined(_WIN32)
        auto* address = ::GetProcAddress(handle_, symbol_name.c_str());
        return reinterpret_cast<void*>(address);
#else
        // POSIX specifies that dlsym's result may be converted to a function
        // pointer. GCC/Clang support the direct reinterpret_cast used here.
        (void)::dlerror();
        return ::dlsym(handle_, symbol_name.c_str());
#endif
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
#if defined(_WIN32)
    HMODULE handle_{nullptr};
#else
    void* handle_{nullptr};
#endif
    std::filesystem::path path_;
};

namespace {

class LoadedDriver final : public IProtocolDriver {
public:
    LoadedDriver(
        std::shared_ptr<DynamicPluginLoader> library,
        IProtocolDriver* implementation,
        DestroyPluginFn destroy)
        : library_(std::move(library)),
          implementation_(implementation),
          destroy_(destroy) {}

    ~LoadedDriver() override {
        if (implementation_ != nullptr) {
            try {
                destroy_(implementation_);
            } catch (...) {
                // A C destroy function must not throw across the ABI.
            }
            implementation_ = nullptr;
        }
    }

    [[nodiscard]] DriverCapabilities capabilities() const override {
        return implementation_->capabilities();
    }

    void configure(const DeviceConfig& device, SampleSink sink) override {
        implementation_->configure(device, std::move(sink));
    }

    void start() override {
        implementation_->start();
    }

    void stop() noexcept override {
        try {
            implementation_->stop();
        } catch (...) {
        }
    }

    [[nodiscard]] RawBatch poll(
        const CollectionGroup& group,
        TimePoint deadline) override {
        return implementation_->poll(group, deadline);
    }

    [[nodiscard]] DeviceControlResult control(
        const DeviceControlRequest& request) override {
        return implementation_->control(request);
    }

private:
    // Declare the handle before the raw pointer. The explicit destructor runs
    // while both members are alive, and the handle is released last.
    std::shared_ptr<DynamicPluginLoader> library_;
    IProtocolDriver* implementation_{nullptr};
    DestroyPluginFn destroy_{nullptr};
};

class LoadedProcessor final : public IDataProcessor {
public:
    LoadedProcessor(
        std::shared_ptr<DynamicPluginLoader> library,
        IDataProcessor* implementation,
        DestroyPluginFn destroy)
        : library_(std::move(library)),
          implementation_(implementation),
          destroy_(destroy) {}

    ~LoadedProcessor() override {
        if (implementation_ != nullptr) {
            try {
                destroy_(implementation_);
            } catch (...) {
            }
            implementation_ = nullptr;
        }
    }

    void process(Event& event, ProcessingContext& context) override {
        implementation_->process(event, context);
    }

private:
    std::shared_ptr<DynamicPluginLoader> library_;
    IDataProcessor* implementation_{nullptr};
    DestroyPluginFn destroy_{nullptr};
};

class LoadedPublisher final : public IEventPublisher {
public:
    LoadedPublisher(
        std::shared_ptr<DynamicPluginLoader> library,
        IEventPublisher* implementation,
        DestroyPluginFn destroy)
        : library_(std::move(library)),
          implementation_(implementation),
          destroy_(destroy) {}

    ~LoadedPublisher() override {
        if (implementation_ != nullptr) {
            try {
                destroy_(implementation_);
            } catch (...) {
            }
            implementation_ = nullptr;
        }
    }

    void configure(const GatewayConfig& gateway) override {
        implementation_->configure(gateway);
    }

    void start() override {
        implementation_->start();
    }

    [[nodiscard]] EventPublishResult publish(const Event& event) override {
        return implementation_->publish(event);
    }

    void stop() noexcept override {
        try {
            implementation_->stop();
        } catch (...) {
        }
    }

private:
    std::shared_ptr<DynamicPluginLoader> library_;
    IEventPublisher* implementation_{nullptr};
    DestroyPluginFn destroy_{nullptr};
};

class LoadedControlSource final : public IDeviceControlSource {
public:
    LoadedControlSource(
        std::shared_ptr<DynamicPluginLoader> library,
        IDeviceControlSource* implementation,
        DestroyPluginFn destroy)
        : library_(std::move(library)),
          implementation_(implementation),
          destroy_(destroy) {}

    ~LoadedControlSource() override {
        if (implementation_ != nullptr) {
            try {
                destroy_(implementation_);
            } catch (...) {
            }
            implementation_ = nullptr;
        }
    }

    void configure(ControlSink submit) override {
        implementation_->configure(std::move(submit));
    }

    void start() override {
        implementation_->start();
    }

    void request_stop() noexcept override {
        try {
            implementation_->request_stop();
        } catch (...) {
        }
    }

    void stop() noexcept override {
        try {
            implementation_->stop();
        } catch (...) {
        }
    }

private:
    // Keep the module loaded until the source object and all of its joined
    // callback activity have been destroyed.
    std::shared_ptr<DynamicPluginLoader> library_;
    IDeviceControlSource* implementation_{nullptr};
    DestroyPluginFn destroy_{nullptr};
};

template <typename FactoryMap>
bool has_factory(const FactoryMap& factories, const std::string& type) {
    return factories.find(type) != factories.end();
}

std::string source_key(PluginKind kind, std::string_view type) {
    std::string category;
    switch (kind) {
        case PluginKind::Driver:
            category = "driver";
            break;
        case PluginKind::Processor:
            category = "processor";
            break;
        case PluginKind::EventPublisher:
            category = "event_publisher";
            break;
        case PluginKind::DeviceControlSource:
            category = "device_control_source";
            break;
    }
    return category + ":" + std::string{type};
}

template <typename Fn>
Fn function_symbol(
    const std::shared_ptr<DynamicPluginLoader>& library,
    std::string_view name,
    std::string_view type) {
    const auto address = library->symbol(name);
    if (address == nullptr) {
        throw std::runtime_error(
            "plugin '" + std::string{type} + "' in " +
            path_text(library->path()) + " does not export " +
            std::string{name});
    }
    return reinterpret_cast<Fn>(address);
}

template <typename Interface, typename Adapter>
std::function<std::unique_ptr<Interface>(std::string_view)> dynamic_factory(
    std::shared_ptr<DynamicPluginLoader> library,
    CreatePluginFn create,
    DestroyPluginFn destroy,
    std::string type) {
    return [library = std::move(library), create, destroy,
            type = std::move(type)](std::string_view settings)
               -> std::unique_ptr<Interface> {
        void* raw = nullptr;
        try {
            const std::string settings_copy{settings};
            raw = create(settings_copy.c_str());
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "create_plugin failed for '" + type + "': " + error.what());
        } catch (...) {
            throw std::runtime_error(
                "create_plugin failed for plugin '" + type + "'");
        }
        if (raw == nullptr) {
            throw std::runtime_error(
                "create_plugin returned null for plugin '" + type + "'");
        }

        try {
            return std::make_unique<Adapter>(
                library,
                static_cast<Interface*>(raw),
                destroy);
        } catch (...) {
            try {
                destroy(raw);
            } catch (...) {
            }
            throw;
        }
    };
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

void PluginRegistry::register_device_control_source(
    std::string type,
    DeviceControlSourceFactory factory) {
    register_factory(
        device_control_source_factories_,
        std::move(type),
        std::move(factory),
        "device control source");
}

void PluginRegistry::load_dynamic_plugins(
    const ApplicationConfig& config) {
    if (dynamic_loading_called_) {
        throw std::logic_error(
            "dynamic plugins can only be loaded once per registry");
    }
    dynamic_loading_called_ = true;

    auto find_or_open = [&](const std::filesystem::path& configured,
                            std::string_view type)
        -> std::shared_ptr<DynamicPluginLoader> {
        validate_library_filename(configured, type);
        for (const auto& loaded : dynamic_plugins_) {
            if (loaded->path() == configured) {
                return loaded;
            }
        }
        try {
            auto loaded = std::make_shared<DynamicPluginLoader>(configured);
            dynamic_plugins_.push_back(loaded);
            return loaded;
        } catch (const std::exception& error) {
            throw std::runtime_error(
                "cannot load plugin '" + std::string{type} + "': " +
                error.what());
        }
    };

    // The minimal C ABI has no category/type descriptor. Keep one shared
    // library tied to one registry key so an implementation cannot
    // accidentally be reinterpreted as a different plugin interface.
    auto claim_library = [&](const std::filesystem::path& configured,
                             const std::string& key) {
        const auto owner = dynamic_library_owners_.find(configured);
        if (owner != dynamic_library_owners_.end() && owner->second != key) {
            throw std::invalid_argument(
                "plugin library " + path_text(configured) +
                " is already assigned to " + owner->second +
                " and cannot be assigned to " + key);
        }
        dynamic_library_owners_.emplace(configured, key);
    };

    auto load_driver = [&](const PluginSpec& spec) {
        const auto key = source_key(PluginKind::Driver, spec.type);
        if (spec.library.empty()) {
            if (!has_factory(driver_factories_, spec.type)) {
                throw std::invalid_argument(
                    "driver plugin '" + spec.type +
                    "' has no library path and is not registered");
            }
            return;
        }
        const auto previous = dynamic_plugin_sources_.find(key);
        if (previous != dynamic_plugin_sources_.end()) {
            if (previous->second != spec.library) {
                throw std::invalid_argument(
                    "driver plugin type is configured with multiple libraries: " +
                    spec.type);
            }
            return;
        }
        if (has_factory(driver_factories_, spec.type)) {
            throw std::invalid_argument(
                "duplicate driver plugin type: " + spec.type);
        }
        claim_library(spec.library, key);
        const auto library = find_or_open(spec.library, spec.type);
        const auto create = function_symbol<CreatePluginFn>(
            library, "create_plugin", spec.type);
        const auto destroy = function_symbol<DestroyPluginFn>(
            library, "destroy_plugin", spec.type);
        register_driver(
            spec.type,
            dynamic_factory<IProtocolDriver, LoadedDriver>(
                library, create, destroy, spec.type));
        dynamic_plugin_sources_.emplace(key, spec.library);
    };

    auto load_processor = [&](const PluginConfig& spec) {
        if (!spec.enabled) {
            return;
        }
        const auto key = source_key(PluginKind::Processor, spec.type);
        if (spec.library.empty()) {
            if (!has_factory(processor_factories_, spec.type)) {
                throw std::invalid_argument(
                    "processor plugin '" + spec.type +
                    "' has no library path and is not registered");
            }
            return;
        }
        const auto previous = dynamic_plugin_sources_.find(key);
        if (previous != dynamic_plugin_sources_.end()) {
            if (previous->second != spec.library) {
                throw std::invalid_argument(
                    "processor plugin type is configured with multiple libraries: " +
                    spec.type);
            }
            return;
        }
        if (has_factory(processor_factories_, spec.type)) {
            throw std::invalid_argument(
                "duplicate processor plugin type: " + spec.type);
        }
        claim_library(spec.library, key);
        const auto library = find_or_open(spec.library, spec.type);
        const auto create = function_symbol<CreatePluginFn>(
            library, "create_plugin", spec.type);
        const auto destroy = function_symbol<DestroyPluginFn>(
            library, "destroy_plugin", spec.type);
        register_processor(
            spec.type,
            dynamic_factory<IDataProcessor, LoadedProcessor>(
                library, create, destroy, spec.type));
        dynamic_plugin_sources_.emplace(key, spec.library);
    };

    auto load_publisher = [&](const PluginConfig& spec) {
        if (!spec.enabled) {
            return;
        }
        const auto key = source_key(PluginKind::EventPublisher, spec.type);
        if (spec.library.empty()) {
            if (!has_factory(event_publisher_factories_, spec.type)) {
                throw std::invalid_argument(
                    "event publisher plugin '" + spec.type +
                    "' has no library path and is not registered");
            }
            return;
        }
        const auto previous = dynamic_plugin_sources_.find(key);
        if (previous != dynamic_plugin_sources_.end()) {
            if (previous->second != spec.library) {
                throw std::invalid_argument(
                    "event publisher type is configured with multiple libraries: " +
                    spec.type);
            }
            return;
        }
        if (has_factory(event_publisher_factories_, spec.type)) {
            throw std::invalid_argument(
                "duplicate event publisher plugin type: " + spec.type);
        }
        claim_library(spec.library, key);
        const auto library = find_or_open(spec.library, spec.type);
        const auto create = function_symbol<CreatePluginFn>(
            library, "create_plugin", spec.type);
        const auto destroy = function_symbol<DestroyPluginFn>(
            library, "destroy_plugin", spec.type);
        register_event_publisher(
            spec.type,
            dynamic_factory<IEventPublisher, LoadedPublisher>(
                library, create, destroy, spec.type));
        dynamic_plugin_sources_.emplace(key, spec.library);
    };

    auto load_control_source = [&](const PluginConfig& spec) {
        if (!spec.enabled) {
            return;
        }
        const auto key = source_key(PluginKind::DeviceControlSource, spec.type);
        if (spec.library.empty()) {
            if (!has_factory(
                    device_control_source_factories_, spec.type)) {
                throw std::invalid_argument(
                    "device control source plugin '" + spec.type +
                    "' has no library path and is not registered");
            }
            return;
        }
        const auto previous = dynamic_plugin_sources_.find(key);
        if (previous != dynamic_plugin_sources_.end()) {
            if (previous->second != spec.library) {
                throw std::invalid_argument(
                    "device control source type is configured with multiple "
                    "libraries: " + spec.type);
            }
            return;
        }
        if (has_factory(device_control_source_factories_, spec.type)) {
            throw std::invalid_argument(
                "duplicate device control source plugin type: " + spec.type);
        }
        claim_library(spec.library, key);
        const auto library = find_or_open(spec.library, spec.type);
        const auto create = function_symbol<CreatePluginFn>(
            library, "create_plugin", spec.type);
        const auto destroy = function_symbol<DestroyPluginFn>(
            library, "destroy_plugin", spec.type);
        register_device_control_source(
            spec.type,
            dynamic_factory<
                IDeviceControlSource,
                LoadedControlSource>(library, create, destroy, spec.type));
        dynamic_plugin_sources_.emplace(key, spec.library);
    };

    for (const auto& device : config.gateway.devices) {
        load_driver(device.driver);
    }
    for (const auto& processor : config.processors) {
        load_processor(processor);
    }
    for (const auto& publisher : config.event_publishers) {
        load_publisher(publisher);
    }
    for (const auto& source : config.device_control_sources) {
        load_control_source(source);
    }
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

    instances.sources.reserve(config.device_control_sources.size());
    for (const auto& spec : config.device_control_sources) {
        if (!spec.enabled) {
            continue;
        }
        const auto& factory = find_factory(
            device_control_source_factories_,
            spec.type,
            "device control source");
        auto source = factory(spec.settings_json);
        if (!source) {
            throw std::runtime_error(
                "device control source plugin factory returned null: " +
                spec.type);
        }
        instances.sources.push_back(std::move(source));
    }
    return instances;
}

}  // namespace gateway
