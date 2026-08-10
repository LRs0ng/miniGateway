#pragma once

#include "gateway/model.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>

namespace gateway {

using ControlArguments = std::unordered_map<std::string, Scalar>;
using ControlClock = std::chrono::steady_clock;

struct DeviceControlRequest {
    std::string request_id;
    std::string device_id;
    std::string command;
    ControlArguments arguments;
    // A default-constructed request has no useful deadline. Treat it as an
    // unbounded deadline so callers that do not need a timeout remain valid.
    ControlClock::time_point deadline{ControlClock::time_point::max()};
};

enum class DeviceControlStatus {
    Succeeded,
    Unsupported,
    InvalidArgument,
    Timeout,
    Cancelled,
    Failed,
};

struct DeviceControlResult {
    std::string request_id;
    DeviceControlStatus status{DeviceControlStatus::Failed};
    ControlArguments outputs;
    std::string message;
};

enum class ControlSubmitResult {
    Accepted,
    QueueFull,
    UnknownDevice,
    InvalidRequest,
    Stopping,
};

using ControlCompletion = std::function<void(DeviceControlResult&&)>;
using ControlSink = std::function<ControlSubmitResult(
    DeviceControlRequest&&,
    ControlCompletion)>;

struct ControlQueueStats {
    std::size_t size{0};
    std::size_t capacity{0};
    std::size_t max_size{0};
    std::uint64_t accepted{0};
    std::uint64_t rejected_full{0};
    std::uint64_t cancelled{0};
};

// Dispatcher counters intentionally describe only generic core operations.
// Protocol-specific counters belong to the source or driver plugin.
struct ControlStats {
    std::uint64_t submitted{0};
    std::uint64_t accepted{0};
    std::uint64_t queue_full{0};
    std::uint64_t unknown_device{0};
    std::uint64_t invalid_requests{0};
    std::uint64_t stopping{0};
    std::uint64_t executed{0};
    std::uint64_t succeeded{0};
    std::uint64_t unsupported{0};
    std::uint64_t invalid_argument{0};
    std::uint64_t timeouts{0};
    std::uint64_t cancelled{0};
    std::uint64_t failed{0};
    std::uint64_t queue_wait_ns{0};
    std::uint64_t execution_ns{0};
};

}  // namespace gateway
