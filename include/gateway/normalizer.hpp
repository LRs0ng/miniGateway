#pragma once

#include "gateway/model.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_map>

namespace gateway {

struct NormalizerStats {
    std::uint64_t batches{0};
    std::uint64_t readings{0};
    std::uint64_t bad_readings{0};
};

class Normalizer {
public:
    explicit Normalizer(const GatewayConfig& config);

    Event normalize(RawBatch batch);
    [[nodiscard]] NormalizerStats stats() const;

private:
    using PointMap = std::unordered_map<std::string, PointConfig>;

    std::unordered_map<std::string, PointMap> points_;
    std::atomic<std::uint64_t> event_sequence_{0};
    std::atomic<std::uint64_t> batches_{0};
    std::atomic<std::uint64_t> readings_{0};
    std::atomic<std::uint64_t> bad_readings_{0};
};

}  // namespace gateway
