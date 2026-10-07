#pragma once

#include "roc/vehicle/result.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>

namespace roc::vehicle {

using Sequence = std::int64_t;

enum class ResourceType { map, road_network };
enum class AcceptedMessageType { heartbeat, telemetry };

struct EnvelopeMetadata {
    std::string message_id;
    Sequence sequence{0};
    std::string timestamp;
};

struct HelloMessage {
    EnvelopeMetadata envelope;
    std::string vehicle_id;
    std::int64_t heartbeat_interval_seconds{0};
    std::int64_t idle_timeout_seconds{0};
    std::int64_t max_message_bytes{0};
    std::int64_t max_heartbeat_bytes{0};
    std::int64_t max_telemetry_bytes{0};
    std::int64_t last_client_sequence{0};
};

struct AckMessage {
    EnvelopeMetadata envelope;
    std::string ack_message_id;
    Sequence ack_sequence{0};
    AcceptedMessageType accepted_type{AcceptedMessageType::heartbeat};
    bool duplicate{false};
};

struct ErrorMessage {
    EnvelopeMetadata envelope;
    std::string code;
    std::string message;
    std::string request_message_id;
};

struct TaskAvailableMessage {
    EnvelopeMetadata envelope;
    std::string task_id;
    std::string batch_id;
    std::string project_id;
    ResourceType resource_type{ResourceType::map};
    std::string resource_revision_id;
    std::int64_t attempt{0};
    std::int64_t max_attempts{0};
};

using DownstreamMessage =
    std::variant<HelloMessage, AckMessage, ErrorMessage, TaskAvailableMessage>;

struct Position {
    double x{0.0};
    double y{0.0};
    double theta{0.0};
};

struct Velocity {
    double linear{0.0};
    double angular{0.0};
};

struct TelemetrySnapshot {
    bool online{false};
    double cpu_usage{0.0};
    double memory_usage{0.0};
    std::int64_t battery_level{0};
    double localization_confidence{0.0};
    Position position;
    Velocity velocity;
    std::string library_version;
};

[[nodiscard]] Result<Sequence> parse_positive_sequence(std::string_view text);
[[nodiscard]] Result<std::int64_t> parse_nonnegative_cursor(std::string_view text);
[[nodiscard]] bool is_valid_uuid(std::string_view text) noexcept;
[[nodiscard]] Result<std::string> generate_uuid_v4();
[[nodiscard]] bool is_valid_rfc3339_timestamp(std::string_view text);

[[nodiscard]] Result<DownstreamMessage> parse_downstream_message(std::string_view json_text);

[[nodiscard]] Result<std::string> make_heartbeat_message(
    std::string_view message_id,
    Sequence sequence,
    std::string_view timestamp,
    std::string_view library_version = {});

[[nodiscard]] Result<std::string> make_telemetry_message(
    std::string_view message_id,
    Sequence sequence,
    std::string_view timestamp,
    const TelemetrySnapshot& telemetry);

}  // namespace roc::vehicle
