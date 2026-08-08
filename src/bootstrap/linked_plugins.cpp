#include "linked_plugins.hpp"

#include "gateway/plugin_registry.hpp"
#include "print_event_publisher.hpp"
#include "processor/processors.hpp"
#include "simulator.hpp"

void register_linked_plugins(gateway::PluginRegistry& registry) {
    gateway::register_poll_simulator_plugin(registry);
    gateway::register_push_simulator_plugin(registry);
    gateway::register_print_event_publisher_plugin(registry);
    gateway::register_threshold_processor_plugin(registry);
    gateway::register_window_average_processor_plugin(registry);
    gateway::register_inference_processor_plugin(registry);

}
