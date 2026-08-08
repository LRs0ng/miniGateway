#include "bootstrap/linked_plugins.hpp"

#include "gateway/config.hpp"
#include "gateway/plugin_registry.hpp"
#include "gateway/runtime.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

std::filesystem::path default_config_path() {
    const std::filesystem::path current_directory_config{"config.json"};
    {
        std::ifstream input{current_directory_config, std::ios::binary};
        if (input.is_open()) {
            return current_directory_config;
        }
    }

    // Avoid depending on argv[0] encoding for non-ASCII Windows paths.
    const std::filesystem::path source_tree_config{"example/config.json"};
    {
        std::ifstream input{source_tree_config, std::ios::binary};
        if (input.is_open()) {
            return source_tree_config;
        }
    }
    return current_directory_config;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: gateway_example [config_path]\n";
        return EXIT_FAILURE;
    }

    try {
        const auto config_path = argc == 2
            ? std::filesystem::path{argv[1]}
            : default_config_path();
        auto config = gateway::load_config(config_path);

        gateway::PluginRegistry registry;
        register_linked_plugins(registry);
        auto plugins = registry.create(config);
        const auto run_for = config.run_duration;

        gateway::GatewayRuntime runtime{
            std::move(config.gateway),
            std::move(plugins.drivers),
            std::move(plugins.processors),
            std::move(plugins.event_publishers)};
        runtime.start();
        std::this_thread::sleep_for(run_for);
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
                  << " publish_errors=" << stats.event_publishers.errors << '\n';
    } catch (const std::exception& error) {
        std::cerr << "gateway failed: " << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
