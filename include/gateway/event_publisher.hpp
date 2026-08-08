#pragma once

#include "gateway/model.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace gateway {

enum class EventPublishResult {
    Accepted,
    Unavailable,
    Rejected,
};

class IEventPublisher {
public:
    virtual ~IEventPublisher() = default;

    virtual void configure(const GatewayConfig& gateway) = 0;
    virtual void start() = 0;
    virtual EventPublishResult publish(const Event& event) = 0;
    virtual void stop() noexcept = 0;
};

struct EventPublisherInstance {
    std::string id;
    std::unique_ptr<IEventPublisher> publisher;
};

struct EventPublisherStats {
    std::uint64_t attempts{0};
    std::uint64_t accepted{0};
    std::uint64_t unavailable{0};
    std::uint64_t rejected{0};
    std::uint64_t errors{0};
};

}  // namespace gateway
