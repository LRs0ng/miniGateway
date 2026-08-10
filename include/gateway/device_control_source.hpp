#pragma once

#include "gateway/control.hpp"

namespace gateway {

// Adapts an external control protocol (MQTT, HTTP, IPC, CLI, ...) to the
// core's bounded device-control queue.  Protocol parsing, authentication,
// authorization, and response formatting remain private to the source.
class IDeviceControlSource {
public:
    virtual ~IDeviceControlSource() = default;

    // The source must retain only the narrow sink it needs to submit requests.
    virtual void configure(ControlSink submit) = 0;
    virtual void start() = 0;
    virtual void request_stop() noexcept = 0;
    virtual void stop() noexcept = 0;
};

}  // namespace gateway
