#pragma once

#include "roc/vehicle/protocol.hpp"
#include "roc/vehicle/result.hpp"
#include "roc/vehicle/state_store.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace roc::vehicle {

enum class SessionDirective {
    none,
    reconnect,
    stop_authentication,
    stop_replaced,
    reject_outbound
};

struct SessionLimits {
    std::int64_t heartbeat_interval_seconds{0};
    std::int64_t idle_timeout_seconds{0};
    std::size_t message_bytes{65536};
    std::size_t heartbeat_bytes{4096};
    std::size_t telemetry_bytes{16384};
};

struct DownstreamEvent {
    SessionDirective directive{SessionDirective::none};
    std::optional<HelloMessage> hello;
    std::optional<AckMessage> ack;
    std::optional<ErrorMessage> error;
    std::optional<TaskAvailableMessage> task;
    bool duplicate_task{false};
};

struct OutboundMessage {
    std::string message_id;
    Sequence sequence{0};
    AcceptedMessageType type{AcceptedMessageType::heartbeat};
    std::string json;
};

class SessionCore {
public:
    SessionCore(DurableStateStore& state_store,
                std::size_t task_queue_capacity = 32,
                SessionLimits local_limits = {},
                std::string expected_vehicle_id = {});

    SessionCore(const SessionCore&) = delete;
    SessionCore& operator=(const SessionCore&) = delete;

    [[nodiscard]] Result<DownstreamEvent> handle_text_frame(std::string_view frame);
    [[nodiscard]] Result<OutboundMessage> make_heartbeat(
        std::string_view timestamp,
        std::string_view library_version = {});
    [[nodiscard]] Result<OutboundMessage> make_telemetry(
        std::string_view timestamp,
        const TelemetrySnapshot& telemetry);

    void reset_connection() noexcept;
    void release_task_notification(std::string_view task_id);
    void release_task_notification(std::string_view task_id, std::int64_t attempt);

    [[nodiscard]] bool ready() const noexcept;
    [[nodiscard]] const SessionLimits& effective_limits() const noexcept;
    [[nodiscard]] const std::string& vehicle_id() const noexcept;
    [[nodiscard]] std::size_t outstanding_count() const noexcept;
    [[nodiscard]] std::size_t pending_task_count() const noexcept;

private:
    struct PendingMessage {
        Sequence sequence{0};
        AcceptedMessageType type{AcceptedMessageType::heartbeat};
    };

    [[nodiscard]] Result<OutboundMessage> track_outbound(
        AcceptedMessageType type,
        std::string message_id,
        Sequence sequence,
        std::string json);

    DurableStateStore& state_store_;
    std::size_t task_queue_capacity_{32};
    SessionLimits local_limits_;
    SessionLimits effective_limits_;
    bool ready_{false};
    std::string vehicle_id_;
    std::string expected_vehicle_id_;
    std::unordered_map<std::string, PendingMessage> outstanding_;
    std::unordered_map<std::string, std::int64_t> pending_tasks_;
};

}  // namespace roc::vehicle
