#include "gateway/device_control_source.hpp"
#include "gateway/plugin_api.hpp"
#include "plugin_support/plugin_json.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace gateway {
namespace {

class PeriodicControlSource final : public IDeviceControlSource {
public:
    PeriodicControlSource(
        std::string request_id_prefix,
        std::vector<std::string> device_ids,
        std::string command,
        std::chrono::milliseconds interval,
        std::chrono::milliseconds timeout)
        : request_id_prefix_(std::move(request_id_prefix)),
          device_ids_(std::move(device_ids)),
          command_(std::move(command)),
          interval_(interval),
          timeout_(timeout) {}

    ~PeriodicControlSource() override {
        stop();
    }

    void configure(ControlSink submit) override {
        if (started_) {
            throw std::logic_error(
                "cannot configure a running periodic control source");
        }
        if (!submit) {
            throw std::invalid_argument(
                "periodic control source requires a sink");
        }
        submit_ = std::move(submit);
        configured_ = true;
    }

    void start() override {
        if (!configured_) {
            throw std::logic_error(
                "periodic control source is not configured");
        }
        if (started_) {
            throw std::logic_error(
                "periodic control source is already started");
        }

        worker_ = std::jthread(
            [this](std::stop_token stop_token) { run(stop_token); });
        started_ = true;
    }

    void request_stop() noexcept override {
        if (worker_.joinable()) {
            worker_.request_stop();
            wakeup_.notify_all();
        }
    }

    void stop() noexcept override {
        request_stop();
        if (worker_.joinable()) {
            worker_.join();
        }

        std::unique_lock lock(mutex_);
        completed_.wait(lock, [this] { return pending_ == 0; });
    }

private:
    void run(std::stop_token stop_token) noexcept {
        std::size_t device_index = 0;
        std::uint64_t sequence = 0;

        while (!stop_token.stop_requested()) {
            ++sequence;
            if (submit(
                    device_ids_[device_index],
                    request_id_prefix_ + "-" + std::to_string(sequence)) ==
                ControlSubmitResult::Stopping) {
                break;
            }
            device_index = (device_index + 1) % device_ids_.size();

            std::unique_lock lock(mutex_);
            if (wakeup_.wait_for(
                    lock,
                    interval_,
                    [&stop_token] { return stop_token.stop_requested(); })) {
                break;
            }
        }
    }

    ControlSubmitResult submit(
        const std::string& device_id,
        std::string request_id) noexcept {
        {
            std::lock_guard lock(mutex_);
            ++pending_;
        }

        try {
            const auto result = submit_(
                DeviceControlRequest{
                    .request_id = std::move(request_id),
                    .device_id = device_id,
                    .command = command_,
                    .arguments = {},
                    .deadline = ControlClock::now() + timeout_,
                },
                [this](DeviceControlResult&&) { complete_one(); });
            if (result != ControlSubmitResult::Accepted) {
                complete_one();
            }
            return result;
        } catch (...) {
            complete_one();
            return ControlSubmitResult::Stopping;
        }
    }

    void complete_one() noexcept {
        {
            std::lock_guard lock(mutex_);
            --pending_;
        }
        completed_.notify_all();
    }

    std::string request_id_prefix_;
    std::vector<std::string> device_ids_;
    std::string command_;
    std::chrono::milliseconds interval_;
    std::chrono::milliseconds timeout_;
    ControlSink submit_;
    std::jthread worker_;
    std::mutex mutex_;
    std::condition_variable wakeup_;
    std::condition_variable completed_;
    std::size_t pending_{0};
    bool configured_{false};
    bool started_{false};
};

std::unique_ptr<PeriodicControlSource> make_control_source(
    std::string_view settings_json) {
    constexpr std::string_view plugin{"periodic control source"};
    const auto settings = plugin_json::parse_object(settings_json, plugin);
    return std::make_unique<PeriodicControlSource>(
        plugin_json::string_member(settings, "request_id_prefix", plugin),
        plugin_json::string_array_member(
            settings, "device_ids", plugin, true),
        plugin_json::string_member(settings, "command", plugin),
        plugin_json::milliseconds_member(
            settings, "interval_ms", plugin, true),
        plugin_json::milliseconds_member(
            settings, "timeout_ms", plugin, true));
}

}  // namespace

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        return make_control_source(
                   settings_json == nullptr ? std::string_view{"{}"}
                                             : std::string_view{settings_json})
            .release();
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<PeriodicControlSource*>(plugin);
}

}  // namespace gateway
