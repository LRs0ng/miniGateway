#pragma once

#include "gateway/bounded_queue.hpp"
#include "gateway/model.hpp"
#include "gateway/scheduler.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace gateway {

enum class AcquisitionMode {
    Poll,
    Push,
};

struct DriverCapabilities {
    AcquisitionMode mode{AcquisitionMode::Poll};
};

using SampleSink = std::function<EnqueueResult(RawBatch&&)>;

class IProtocolDriver {
public:
    virtual ~IProtocolDriver() = default;

    virtual DriverCapabilities capabilities() const = 0;
    virtual void configure(const DeviceConfig& device, SampleSink sink) = 0;
    virtual void start() = 0;
    virtual void stop() noexcept = 0;

    virtual RawBatch poll(const CollectionGroup&, TimePoint) {
        throw std::logic_error("driver does not support polling");
    }
};

struct DriverInstance {
    std::string device_id;
    std::unique_ptr<IProtocolDriver> driver;
};

struct AcquisitionStats {
    std::uint64_t polls{0};
    std::uint64_t errors{0};
    std::uint64_t deadline_misses{0};
    std::uint64_t queue_full{0};
};

}  // namespace gateway
