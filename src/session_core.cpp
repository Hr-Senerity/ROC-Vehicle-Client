#include "session_core.hpp"

#include <algorithm>
#include <limits>
#include <utility>

namespace roc::vehicle {
namespace {

Error session_error(ErrorCode code, bool retryable, std::string message) {
    return Error{ErrorDomain::protocol, code, retryable, std::move(message)};
}

std::size_t as_size(std::int64_t value) {
    return static_cast<std::size_t>(value);
}

}  // namespace

SessionCore::SessionCore(DurableStateStore& state_store,
                         std::size_t task_queue_capacity,
                         SessionLimits local_limits,
                         std::string expected_vehicle_id)
    : state_store_(state_store),
      task_queue_capacity_(task_queue_capacity),
      local_limits_(local_limits),
      effective_limits_(local_limits),
      expected_vehicle_id_(std::move(expected_vehicle_id)) {}

Result<DownstreamEvent> SessionCore::handle_text_frame(std::string_view frame) {
    const auto parsed = parse_downstream_message(frame);
    if (!parsed) {
        return parsed.error();
    }

    DownstreamEvent event;
    if (const auto* hello = std::get_if<HelloMessage>(&parsed.value())) {
        if (ready_) {
            return session_error(ErrorCode::invalid_state, false,
                                 "hello is only valid as the first session message");
        }
        if (!expected_vehicle_id_.empty() && hello->vehicle_id != expected_vehicle_id_) {
            return Error{ErrorDomain::authentication, ErrorCode::permission_denied,
                         false, "hello vehicle identity differs from pinned identity",
                         "vehicle_identity_mismatch"};
        }
        const auto aligned = state_store_.align_server_sequence(
            hello->last_client_sequence);
        if (!aligned) {
            return aligned.error();
        }
        effective_limits_.heartbeat_interval_seconds =
            hello->heartbeat_interval_seconds;
        effective_limits_.idle_timeout_seconds = hello->idle_timeout_seconds;
        effective_limits_.message_bytes = std::min(
            local_limits_.message_bytes, as_size(hello->max_message_bytes));
        effective_limits_.heartbeat_bytes = std::min(
            local_limits_.heartbeat_bytes, as_size(hello->max_heartbeat_bytes));
        effective_limits_.telemetry_bytes = std::min(
            local_limits_.telemetry_bytes, as_size(hello->max_telemetry_bytes));
        vehicle_id_ = hello->vehicle_id;
        ready_ = true;
        event.hello = *hello;
        return event;
    }

    if (const auto* error = std::get_if<ErrorMessage>(&parsed.value())) {
        event.error = *error;
        if (error->code == "authentication_failed" ||
            error->code == "authentication_required" ||
            error->code == "credential_revoked" ||
            error->code == "credential_rotated") {
            event.directive = SessionDirective::stop_authentication;
        } else if (error->code == "connection_replaced") {
            event.directive = SessionDirective::stop_replaced;
        } else if (error->code == "heartbeat_timeout" ||
                   error->code == "capacity_reached" ||
                   error->code == "service_unavailable") {
            event.directive = SessionDirective::reconnect;
        } else if (error->code == "message_too_large" ||
                   error->code == "invalid_library_version" ||
                   error->code == "message_rejected") {
            event.directive = SessionDirective::reject_outbound;
            if (!error->request_message_id.empty()) {
                outstanding_.erase(error->request_message_id);
            }
        }
        return event;
    }

    if (!ready_) {
        return session_error(ErrorCode::invalid_state, true,
                             "server message arrived before hello");
    }

    if (const auto* ack = std::get_if<AckMessage>(&parsed.value())) {
        const auto pending = outstanding_.find(ack->ack_message_id);
        if (pending == outstanding_.end() || pending->second.sequence != ack->ack_sequence ||
            pending->second.type != ack->accepted_type) {
            return session_error(ErrorCode::invalid_state, false,
                                 "ack does not match an outstanding message");
        }
        outstanding_.erase(pending);
        event.ack = *ack;
        return event;
    }

    const auto& task = std::get<TaskAvailableMessage>(parsed.value());
    const auto pending = pending_tasks_.find(task.task_id);
    if (pending != pending_tasks_.end() && task.attempt <= pending->second) {
        event.duplicate_task = true;
        return event;
    }
    if (pending == pending_tasks_.end() && pending_tasks_.size() >= task_queue_capacity_) {
        return session_error(ErrorCode::unavailable, true,
                             "task notification queue is full");
    }
    pending_tasks_[task.task_id] = task.attempt;
    event.task = task;
    return event;
}

Result<OutboundMessage> SessionCore::make_heartbeat(
    std::string_view timestamp,
    std::string_view library_version) {
    if (!ready_) {
        return session_error(ErrorCode::invalid_state, true,
                             "heartbeat cannot be sent before hello");
    }
    const auto message_id = generate_uuid_v4();
    if (!message_id) {
        return message_id.error();
    }
    const auto sequence = state_store_.allocate_sequence();
    if (!sequence) {
        return sequence.error();
    }
    auto json = make_heartbeat_message(message_id.value(), sequence.value(), timestamp,
                                       library_version);
    if (!json) {
        return json.error();
    }
    if (json.value().size() > effective_limits_.heartbeat_bytes ||
        json.value().size() > effective_limits_.message_bytes) {
        return session_error(ErrorCode::invalid_argument, false,
                             "heartbeat exceeds negotiated session limit");
    }
    return track_outbound(AcceptedMessageType::heartbeat, message_id.value(),
                          sequence.value(), std::move(json.value()));
}

Result<OutboundMessage> SessionCore::make_telemetry(
    std::string_view timestamp,
    const TelemetrySnapshot& telemetry) {
    if (!ready_) {
        return session_error(ErrorCode::invalid_state, true,
                             "telemetry cannot be sent before hello");
    }
    const auto message_id = generate_uuid_v4();
    if (!message_id) {
        return message_id.error();
    }
    const auto sequence = state_store_.allocate_sequence();
    if (!sequence) {
        return sequence.error();
    }
    auto json = make_telemetry_message(message_id.value(), sequence.value(), timestamp,
                                       telemetry);
    if (!json) {
        return json.error();
    }
    if (json.value().size() > effective_limits_.telemetry_bytes ||
        json.value().size() > effective_limits_.message_bytes) {
        return session_error(ErrorCode::invalid_argument, false,
                             "telemetry exceeds negotiated session limit");
    }
    return track_outbound(AcceptedMessageType::telemetry, message_id.value(),
                          sequence.value(), std::move(json.value()));
}

void SessionCore::reset_connection() noexcept {
    ready_ = false;
    vehicle_id_.clear();
    effective_limits_ = local_limits_;
    outstanding_.clear();
}

void SessionCore::release_task_notification(std::string_view task_id) {
    pending_tasks_.erase(std::string{task_id});
}

void SessionCore::release_task_notification(std::string_view task_id, std::int64_t attempt) {
    const auto found = pending_tasks_.find(std::string{task_id});
    if (found != pending_tasks_.end() && found->second == attempt) pending_tasks_.erase(found);
}

bool SessionCore::ready() const noexcept {
    return ready_;
}

const SessionLimits& SessionCore::effective_limits() const noexcept {
    return effective_limits_;
}

const std::string& SessionCore::vehicle_id() const noexcept {
    return vehicle_id_;
}

std::size_t SessionCore::outstanding_count() const noexcept {
    return outstanding_.size();
}

std::size_t SessionCore::pending_task_count() const noexcept {
    return pending_tasks_.size();
}

Result<OutboundMessage> SessionCore::track_outbound(
    AcceptedMessageType type,
    std::string message_id,
    Sequence sequence,
    std::string json) {
    constexpr std::size_t maximum_outstanding_messages = 256;
    if (outstanding_.size() >= maximum_outstanding_messages) {
        return session_error(ErrorCode::unavailable, true,
                             "outstanding ACK window is full");
    }
    const auto inserted = outstanding_.emplace(
        message_id, PendingMessage{sequence, type});
    if (!inserted.second) {
        return session_error(ErrorCode::internal_failure, false,
                             "generated duplicate message_id");
    }
    return OutboundMessage{std::move(message_id), sequence, type, std::move(json)};
}

}  // namespace roc::vehicle
