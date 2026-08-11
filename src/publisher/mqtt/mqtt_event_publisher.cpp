#include "mqtt/mqtt_event_publisher.hpp"

#include "plugin_support/plugin_json.hpp"
#include "gateway/plugin_api.hpp"

#include <chrono>
#include <cmath>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>

namespace gateway {
namespace {

std::runtime_error paho_error(std::string_view operation, int rc) {
    std::ostringstream message;
    message << operation << " failed (" << rc;
    if (const auto* description = MQTTAsync_strerror(rc);
        description != nullptr) {
        message << ": " << description;
    }
    message << ')';
    return std::runtime_error{message.str()};
}

bool is_temporarily_unavailable(int rc) noexcept {
    return rc == MQTTASYNC_DISCONNECTED ||
           rc == MQTTASYNC_MAX_MESSAGES_INFLIGHT ||
           rc == MQTTASYNC_NO_MORE_MSGIDS ||
           rc == MQTTASYNC_OPERATION_INCOMPLETE ||
           rc == MQTTASYNC_MAX_BUFFERED_MESSAGES;
}

void append_json_string(std::ostringstream& output, std::string_view value) {
    static constexpr char hex[] = "0123456789abcdef";
    output.put('"');
    for (const unsigned char character : value) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20U) {
                    output << "\\u00" << hex[(character >> 4U) & 0x0fU]
                           << hex[character & 0x0fU];
                } else {
                    output.put(static_cast<char>(character));
                }
                break;
        }
    }
    output.put('"');
}

void append_json_scalar(std::ostringstream& output, const Scalar& value) {
    std::visit(
        [&output](const auto& item) {
            using T = std::decay_t<decltype(item)>;
            if constexpr (std::is_same_v<T, std::int64_t>) {
                output << item;
            } else if constexpr (std::is_same_v<T, double>) {
                if (std::isfinite(item)) {
                    output << std::setprecision(17) << item;
                } else {
                    output << "null";
                }
            } else if constexpr (std::is_same_v<T, bool>) {
                output << (item ? "true" : "false");
            } else {
                append_json_string(output, item);
            }
        },
        value);
}

}  // namespace

MqttEventPublisher::MqttEventPublisher(MqttPublisherConfig config)
    : config_(std::move(config)) {
    validate_config();
}

MqttEventPublisher::~MqttEventPublisher() {
    stop();
}

void MqttEventPublisher::configure(const GatewayConfig& gateway) {
    std::lock_guard lock(mutex_);
    if (started_) {
        throw std::logic_error(
            "cannot configure a running MQTT event publisher");
    }
    validate_config();
    for (const auto& device : gateway.devices) {
        if (device.id.find_first_of("+#") != std::string::npos ||
            device.id.find('\0') != std::string::npos) {
            throw std::invalid_argument(
                "MQTT topic device id must not contain +, # or NUL: " +
                device.id);
        }
    }
    configured_ = true;
}

void MqttEventPublisher::start() {
    std::lock_guard lock(mutex_);
    if (!configured_) {
        throw std::logic_error("MQTT event publisher is not configured");
    }
    if (started_) {
        throw std::logic_error("MQTT event publisher is already running");
    }

    const auto uri = server_uri();
    // MQTTAsync_create() is the smallest Paho client construction path. With
    // MQTTCLIENT_PERSISTENCE_NONE it has no file persistence, and Paho rejects
    // sends while disconnected instead of keeping a second offline queue.
    client_ = nullptr;
    int rc = MQTTAsync_create(
        &client_, uri.c_str(), config_.client_id.c_str(),
        MQTTCLIENT_PERSISTENCE_NONE, nullptr);
    if (rc != MQTTASYNC_SUCCESS) {
        // Paho may leave an opaque, partially initialized handle on an
        // allocation failure. Its destroy routine expects a fully initialized
        // client, so discard the handle rather than calling that failure path.
        client_ = nullptr;
        started_ = false;
        connected_.store(false, std::memory_order_relaxed);
        throw paho_error("MQTTAsync_create", rc);
    }

    auto cleanup = [this] {
        if (client_ != nullptr) {
            MQTTAsync_destroy(&client_);
        }
        started_ = false;
        connected_.store(false, std::memory_order_relaxed);
    };

    rc = MQTTAsync_setConnectionLostCallback(
        client_, this, &MqttEventPublisher::on_connection_lost);
    if (rc != MQTTASYNC_SUCCESS) {
        cleanup();
        throw paho_error("MQTTAsync_setConnectionLostCallback", rc);
    }
    rc = MQTTAsync_setConnected(
        client_, this, &MqttEventPublisher::on_reconnected);
    if (rc != MQTTASYNC_SUCCESS) {
        cleanup();
        throw paho_error("MQTTAsync_setConnected", rc);
    }

    MQTTAsync_connectOptions connect_options =
        MQTTAsync_connectOptions_initializer;
    connect_options.keepAliveInterval = config_.keepalive;
    connect_options.cleansession = config_.clean_session ? 1 : 0;
    connect_options.username =
        config_.username.empty() ? nullptr : config_.username.c_str();
    connect_options.password =
        config_.password.empty() ? nullptr : config_.password.c_str();
    connect_options.MQTTVersion = MQTTVERSION_3_1_1;
    connect_options.automaticReconnect = 1;
    connect_options.minRetryInterval =
        static_cast<int>(config_.reconnect_delay);
    connect_options.maxRetryInterval =
        static_cast<int>(config_.reconnect_delay_max);
    connect_options.context = this;
    connect_options.onSuccess = &MqttEventPublisher::on_connect_success;
    connect_options.onFailure = &MqttEventPublisher::on_connect_failure;

    rc = MQTTAsync_connect(client_, &connect_options);
    if (rc != MQTTASYNC_SUCCESS) {
        cleanup();
        throw paho_error("MQTTAsync_connect", rc);
    }
    started_ = true;
}

EventPublishResult MqttEventPublisher::publish(const Event& event) {
    const auto payload = event_to_json(event);
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        return EventPublishResult::Rejected;
    }
    const auto topic = topic_for(event);

    std::lock_guard lock(mutex_);
    if (!started_ || client_ == nullptr) {
        unavailable_.fetch_add(1, std::memory_order_relaxed);
        return EventPublishResult::Unavailable;
    }

    MQTTAsync_responseOptions response =
        MQTTAsync_responseOptions_initializer;
    response.context = this;
    response.onSuccess = &MqttEventPublisher::on_publish_success;
    const auto rc = MQTTAsync_send(
        client_, topic.c_str(), static_cast<int>(payload.size()),
        payload.data(), config_.qos, config_.retain ? 1 : 0, &response);
    if (rc == MQTTASYNC_SUCCESS) {
        accepted_.fetch_add(1, std::memory_order_relaxed);
        return EventPublishResult::Accepted;
    }
    if (is_temporarily_unavailable(rc)) {
        unavailable_.fetch_add(1, std::memory_order_relaxed);
        return EventPublishResult::Unavailable;
    }
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return EventPublishResult::Rejected;
}

void MqttEventPublisher::stop() noexcept {
    std::lock_guard lock(mutex_);
    if (client_ == nullptr) {
        started_ = false;
        connected_.store(false, std::memory_order_relaxed);
        return;
    }

    started_ = false;
    connected_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard disconnect_lock(disconnect_mutex_);
        disconnect_complete_ = false;
    }

    MQTTAsync_disconnectOptions disconnect_options =
        MQTTAsync_disconnectOptions_initializer;
    disconnect_options.timeout = 500;
    disconnect_options.context = this;
    disconnect_options.onSuccess = &MqttEventPublisher::on_disconnect_success;
    disconnect_options.onFailure = &MqttEventPublisher::on_disconnect_failure;

    const auto rc = MQTTAsync_disconnect(client_, &disconnect_options);
    if (rc == MQTTASYNC_SUCCESS) {
        std::unique_lock disconnect_lock(disconnect_mutex_);
        (void)disconnect_cv_.wait_for(
            disconnect_lock, std::chrono::milliseconds{1000},
            [this] { return disconnect_complete_; });
    }

    MQTTAsync_destroy(&client_);
    // A connect/reconnect callback may have raced with the disconnect wait.
    // The handle is gone now, so the externally visible state is definitely
    // disconnected.
    connected_.store(false, std::memory_order_relaxed);
}

MqttPublisherStats MqttEventPublisher::stats() const {
    return MqttPublisherStats{
        .accepted = accepted_.load(std::memory_order_relaxed),
        .acknowledged = acknowledged_.load(std::memory_order_relaxed),
        .unavailable = unavailable_.load(std::memory_order_relaxed),
        .rejected = rejected_.load(std::memory_order_relaxed),
        .connected = connected_.load(std::memory_order_relaxed),
    };
}

void MqttEventPublisher::on_connect_success(
    void* context,
    MQTTAsync_successData*) {
    auto* plugin = static_cast<MqttEventPublisher*>(context);
    if (plugin != nullptr) {
        plugin->connected_.store(true, std::memory_order_relaxed);
    }
}

void MqttEventPublisher::on_connect_failure(
    void* context,
    MQTTAsync_failureData*) {
    auto* plugin = static_cast<MqttEventPublisher*>(context);
    if (plugin != nullptr) {
        plugin->connected_.store(false, std::memory_order_relaxed);
    }
}

void MqttEventPublisher::on_connection_lost(void* context, char*) {
    auto* plugin = static_cast<MqttEventPublisher*>(context);
    if (plugin != nullptr) {
        plugin->connected_.store(false, std::memory_order_relaxed);
    }
}

void MqttEventPublisher::on_reconnected(void* context, char*) {
    auto* plugin = static_cast<MqttEventPublisher*>(context);
    if (plugin != nullptr) {
        plugin->connected_.store(true, std::memory_order_relaxed);
    }
}

void MqttEventPublisher::on_publish_success(
    void* context,
    MQTTAsync_successData*) {
    auto* plugin = static_cast<MqttEventPublisher*>(context);
    if (plugin != nullptr) {
        plugin->acknowledged_.fetch_add(1, std::memory_order_relaxed);
    }
}

void MqttEventPublisher::on_disconnect_success(
    void* context,
    MQTTAsync_successData*) {
    auto* plugin = static_cast<MqttEventPublisher*>(context);
    if (plugin != nullptr) {
        plugin->signal_disconnect_complete();
    }
}

void MqttEventPublisher::on_disconnect_failure(
    void* context,
    MQTTAsync_failureData*) {
    auto* plugin = static_cast<MqttEventPublisher*>(context);
    if (plugin != nullptr) {
        plugin->signal_disconnect_complete();
    }
}

std::string MqttEventPublisher::server_uri() const {
    std::string host = config_.host;
    if (host.find(':') != std::string::npos &&
        !(host.size() >= 2 && host.front() == '[' && host.back() == ']')) {
        host.insert(host.begin(), '[');
        host.push_back(']');
    }
    return "tcp://" + host + ':' + std::to_string(config_.port);
}

std::string MqttEventPublisher::topic_for(const Event& event) const {
    std::string topic = config_.topic_prefix;
    if (topic.back() != '/') {
        topic.push_back('/');
    }
    topic += event.device_id;
    topic += "/event";
    return topic;
}

void MqttEventPublisher::validate_config() const {
    if (config_.host.empty() || config_.client_id.empty() ||
        config_.topic_prefix.empty()) {
        throw std::invalid_argument(
            "MQTT host, client_id and topic_prefix are required");
    }
    if (config_.port <= 0 || config_.port > 65535) {
        throw std::invalid_argument("MQTT port must be between 1 and 65535");
    }
    if (config_.keepalive < 0 || config_.keepalive == 1 ||
        config_.keepalive > 65535) {
        throw std::invalid_argument(
            "MQTT keepalive must be 0 or in range 2..65535");
    }
    if (config_.topic_prefix.find_first_of("+#") != std::string::npos ||
        config_.topic_prefix.find('\0') != std::string::npos) {
        throw std::invalid_argument(
            "MQTT topic_prefix must not contain +, # or NUL");
    }
    if (config_.qos < 0 || config_.qos > 2) {
        throw std::invalid_argument("MQTT QoS must be 0, 1 or 2");
    }
    const auto max_retry =
        static_cast<unsigned>(std::numeric_limits<int>::max());
    if (config_.reconnect_delay == 0 ||
        config_.reconnect_delay_max < config_.reconnect_delay ||
        config_.reconnect_delay_max > max_retry) {
        throw std::invalid_argument(
            "MQTT reconnect delays must satisfy 1 <= delay <= delay_max <= INT_MAX");
    }
    if (!config_.reconnect_exponential_backoff) {
        throw std::invalid_argument(
            "Paho MQTTAsync requires exponential reconnect backoff");
    }
}

void MqttEventPublisher::signal_disconnect_complete() noexcept {
    {
        std::lock_guard lock(disconnect_mutex_);
        disconnect_complete_ = true;
    }
    disconnect_cv_.notify_all();
}

std::string event_to_json(const Event& event) {
    std::ostringstream output;
    output << "{\"event_id\":";
    append_json_string(output, event.event_id);
    output << ",\"device_id\":";
    append_json_string(output, event.device_id);
    output << ",\"source\":";
    append_json_string(output, event.source);
    output << ",\"model_version\":";
    append_json_string(output, event.model_version);
    output << ",\"readings\":[";

    for (std::size_t index = 0; index < event.readings.size(); ++index) {
        if (index != 0) {
            output.put(',');
        }
        const auto& reading = event.readings[index];
        output << "{\"point\":";
        append_json_string(output, reading.point);
        output << ",\"value\":";
        append_json_scalar(output, reading.value);
        output << ",\"quality\":";
        append_json_string(output, quality_name(reading.quality));
        output << ",\"unit\":";
        append_json_string(output, reading.unit);
        output << ",\"source_time_ns\":" << reading.source_time_ns;
        output << ",\"received_time_ns\":" << reading.received_time_ns;
        output << ",\"derived\":"
               << (reading.derived ? "true" : "false") << '}';
    }
    output << "]}";
    return output.str();
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void* create_plugin(
    const char* settings_json) {
    try {
        constexpr std::string_view plugin{"MQTT event publisher"};
        const auto settings = plugin_json::parse_object(
            settings_json == nullptr ? std::string_view{"{}"}
                                      : std::string_view{settings_json},
            plugin);
        MqttPublisherConfig config{
            .host = plugin_json::string_member(settings, "host", plugin),
            .port = plugin_json::int_member(settings, "port", plugin, 1, 65535),
            .keepalive = plugin_json::int_member(
                settings, "keepalive", plugin, 0, 65535),
            .client_id = plugin_json::string_member(
                settings, "client_id", plugin),
            .topic_prefix = plugin_json::string_member(
                settings, "topic_prefix", plugin),
            .qos = plugin_json::int_member(settings, "qos", plugin, 0, 2),
            .retain = plugin_json::bool_member(settings, "retain", plugin),
            .clean_session = plugin_json::bool_member(
                settings, "clean_session", plugin),
            .username = plugin_json::string_member(
                settings, "username", plugin, true),
            .password = plugin_json::string_member(
                settings, "password", plugin, true),
            .reconnect_delay = plugin_json::unsigned_member(
                settings, "reconnect_delay", plugin),
            .reconnect_delay_max = plugin_json::unsigned_member(
                settings, "reconnect_delay_max", plugin),
            .reconnect_exponential_backoff = plugin_json::bool_member(
                settings, "reconnect_exponential_backoff", plugin),
        };
        return std::make_unique<MqttEventPublisher>(std::move(config)).release();
    } catch (...) {
        return nullptr;
    }
}

GATEWAY_PLUGIN_C GATEWAY_PLUGIN_EXPORT void destroy_plugin(void* plugin) {
    delete static_cast<MqttEventPublisher*>(plugin);
}

}  // namespace gateway
