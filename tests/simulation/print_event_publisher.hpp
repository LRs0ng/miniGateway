#pragma once

#include "gateway/event_publisher.hpp"
#include "gateway/plugin_registry.hpp"

#include <cstdint>

namespace gateway {

class PrintEventPublisher final : public IEventPublisher {
public:
    explicit PrintEventPublisher(bool include_readings = true);

    void configure(const GatewayConfig& gateway) override;
    void start() override;
    [[nodiscard]] EventPublishResult publish(const Event& event) override;
    void stop() noexcept override;

    [[nodiscard]] std::uint64_t published() const noexcept;

private:
    bool include_readings_{true};
    bool configured_{false};
    bool started_{false};
    std::uint64_t published_{0};
};

void register_print_event_publisher_plugin(PluginRegistry& registry);

}  // namespace gateway
