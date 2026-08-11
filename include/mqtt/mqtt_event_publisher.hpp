#pragma once

#include "gateway/event_publisher.hpp"

#include <MQTTAsync.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>

namespace gateway {

struct MqttPublisherConfig {
    std::string host{"127.0.0.1"};
    int port{1883};
    int keepalive{30};
    std::string client_id{"edge-gateway"};
    std::string topic_prefix{"edge/events"};
    int qos{1};
    bool retain{false};
    bool clean_session{true};
    std::string username;
    std::string password;
    unsigned reconnect_delay{1};
    unsigned reconnect_delay_max{30};
    bool reconnect_exponential_backoff{true};
};

struct MqttPublisherStats {
    std::uint64_t accepted{0};
    std::uint64_t acknowledged{0};
    std::uint64_t unavailable{0};
    std::uint64_t rejected{0};
    bool connected{false};
};

class MqttEventPublisher final : public IEventPublisher {
public:
    explicit MqttEventPublisher(MqttPublisherConfig config = {});
    ~MqttEventPublisher() override;

    void configure(const GatewayConfig& gateway) override;
    void start() override;
    [[nodiscard]] EventPublishResult publish(const Event& event) override;
    void stop() noexcept override;

    [[nodiscard]] MqttPublisherStats stats() const;

private:
    static void on_connect_success(
        void* context,
        ::MQTTAsync_successData* response);
    static void on_connect_failure(
        void* context,
        ::MQTTAsync_failureData* response);
    static void on_connection_lost(void* context, char* cause);
    static void on_reconnected(void* context, char* cause);
    static void on_publish_success(
        void* context,
        ::MQTTAsync_successData* response);
    static void on_disconnect_success(
        void* context,
        ::MQTTAsync_successData* response);
    static void on_disconnect_failure(
        void* context,
        ::MQTTAsync_failureData* response);

    [[nodiscard]] std::string server_uri() const;
    [[nodiscard]] std::string topic_for(const Event& event) const;
    void validate_config() const;
    void signal_disconnect_complete() noexcept;

    MqttPublisherConfig config_;
    mutable std::mutex mutex_;
    std::mutex disconnect_mutex_;
    std::condition_variable disconnect_cv_;
    ::MQTTAsync client_{nullptr};
    bool disconnect_complete_{false};
    bool configured_{false};
    bool started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> acknowledged_{0};
    std::atomic<std::uint64_t> unavailable_{0};
    std::atomic<std::uint64_t> rejected_{0};
};

[[nodiscard]] std::string event_to_json(const Event& event);

}  // namespace gateway
