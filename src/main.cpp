#include "gateway/config.hpp"
#include "gateway/plugin_registry.hpp"
#include "gateway/runtime.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace {

volatile std::sig_atomic_t shutdown_requested = 0;

void request_stop(int) noexcept {
    shutdown_requested = 1;
}

void install_signal_handlers() noexcept {
    (void)std::signal(SIGINT, request_stop);
    (void)std::signal(SIGTERM, request_stop);
}

void wait_for_shutdown(
    const std::optional<std::chrono::milliseconds>& run_duration) {
    constexpr auto check_interval = std::chrono::milliseconds{100};

    if (!run_duration) {
        while (shutdown_requested == 0) {
            std::this_thread::sleep_for(check_interval);
        }
        return;
    }

    const auto deadline = std::chrono::steady_clock::now() + *run_duration;
    while (shutdown_requested == 0) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            return;
        }
        const auto next_check = now + check_interval;
        std::this_thread::sleep_until(
            next_check < deadline ? next_check : deadline);
    }
}

std::filesystem::path default_config_path() {
    return std::filesystem::path{"config.json"};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: gateway_example [config_path]\n";
        return EXIT_FAILURE;
    }

    install_signal_handlers();

    try {
        const auto config_path = argc == 2
            ? std::filesystem::path{argv[1]}
            : default_config_path();
        auto config = gateway::load_config(config_path);

        gateway::PluginInstances plugins;
        {
            // The registry is a startup-only assembler. Loaded adapters keep
            // their own shared-library handles after this scope ends.
            gateway::PluginRegistry registry;
            registry.load_dynamic_plugins(config);
            plugins = registry.create(config);
        }
        const auto run_for = config.run_duration;

        gateway::GatewayRuntime runtime{
            std::move(config.gateway),
            std::move(plugins.drivers),
            std::move(plugins.processors),
            std::move(plugins.event_publishers),
            std::move(plugins.sources)};
        runtime.start();
        wait_for_shutdown(run_for);
        runtime.stop();

        const auto stats = runtime.stats();
        std::cout << "\nsummary: events=" << stats.delivered_events
                  << " queue_full=" << stats.raw_queue.rejected_full
                  << " polls=" << stats.acquisition.polls
                  << " poll_errors=" << stats.acquisition.errors
                  << " deadline_misses=" << stats.acquisition.deadline_misses
                  << " skipped=" << stats.scheduler.skipped_cycles
                  << " processor_errors=" << stats.processing.processor_errors
                  << " publish_accepted=" << stats.event_publishers.accepted
                  << " publish_unavailable=" << stats.event_publishers.unavailable
                  << " publish_errors=" << stats.event_publishers.errors
                  << " control_accepted=" << stats.control.accepted
                  << " control_succeeded=" << stats.control.succeeded
                  << " control_failed=" << stats.control.failed
                  << " control_cancelled=" << stats.control.cancelled << '\n';
    } catch (const std::exception& error) {
        std::cerr << "gateway failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
