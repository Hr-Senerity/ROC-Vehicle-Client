#pragma once

#include "roc/vehicle/protocol.hpp"
#include "roc/vehicle/result.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace roc::vehicle {

enum class TaskExecutionState {
    accepted,
    downloading,
    delivering,
    delivered,
    failed,
    reconciliation_required,
    expired,
    canceled
};

enum class StatusEventDisposition {
    pending,
    acknowledged,
    reconciliation_required,
    resolved,
    forensic
};

struct TaskGenerationKey {
    std::string task_id;
    std::int64_t attempt{0};
};

struct TaskGenerationState {
    TaskGenerationKey key;
    TaskExecutionState state{TaskExecutionState::accepted};
    std::int64_t progress_floor{0};
    std::int64_t progress{0};
};

struct ActiveTaskLease {
    TaskAvailableMessage notification;
    TaskGenerationKey generation;
    std::string lease_token;
    std::int64_t lease_expires_at{0};
    std::int64_t progress_floor{0};
};

struct StatusEventRecord {
    std::string event_id;
    TaskGenerationKey generation;
    TaskExecutionState state{TaskExecutionState::downloading};
    std::int64_t progress{0};
    std::string error_code;
    std::string error_message;
    std::string payload_json;
    std::string request_sha256;
    std::string lease_token;
    std::int64_t lease_expires_at{0};
    std::string local_delivery_evidence;
    StatusEventDisposition disposition{StatusEventDisposition::pending};
    std::string reconciliation_reason;
};

class DurableStateStore {
public:
    static Result<std::unique_ptr<DurableStateStore>> open(
        const std::filesystem::path& database_path,
        std::uint32_t sequence_reservation_size = 64);

    DurableStateStore(const DurableStateStore&) = delete;
    DurableStateStore& operator=(const DurableStateStore&) = delete;
    DurableStateStore(DurableStateStore&&) = delete;
    DurableStateStore& operator=(DurableStateStore&&) = delete;

    ~DurableStateStore();

    [[nodiscard]] Result<void> align_server_sequence(std::int64_t last_client_sequence);
    [[nodiscard]] Result<Sequence> allocate_sequence();
    [[nodiscard]] std::int64_t reserved_through() const;

    [[nodiscard]] Result<TaskGenerationState> register_task_generation(
        const TaskGenerationKey& key,
        std::int64_t accepted_progress_floor);
    [[nodiscard]] Result<TaskGenerationState> task_generation(
        const TaskGenerationKey& key) const;
    // Save immediately after accept and before download/status. This protected
    // record survives acknowledgement of nonterminal Outbox events.
    [[nodiscard]] Result<void> persist_active_task_lease(const ActiveTaskLease& lease);
    [[nodiscard]] Result<ActiveTaskLease> active_task_lease(
        const TaskGenerationKey& key) const;
    [[nodiscard]] Result<std::vector<TaskGenerationKey>> active_task_keys() const;
    // Legacy/partial state must not repair downloading/delivering via accept.
    [[nodiscard]] Result<bool> has_unrecoverable_active_generation(
        std::string_view task_id) const;
    [[nodiscard]] Result<void> mark_task_generation_expired(
        const TaskGenerationKey& key);
    [[nodiscard]] Result<void> persist_status_event(const StatusEventRecord& event);
    [[nodiscard]] Result<StatusEventRecord> status_event(
        std::string_view event_id) const;
    [[nodiscard]] Result<void> acknowledge_status_event(std::string_view event_id);
    [[nodiscard]] Result<void> mark_status_event_reconciliation_required(
        std::string_view event_id,
        std::string_view reason);
    [[nodiscard]] Result<void> resolve_status_event_reconciliation(
        std::string_view event_id);
    [[nodiscard]] Result<std::vector<StatusEventRecord>> pending_status_events() const;
    [[nodiscard]] Result<std::vector<StatusEventRecord>>
    reconciliation_required_events() const;
    [[nodiscard]] Result<bool> has_unconfirmed_terminal_event(
        std::string_view task_id) const;

private:
    struct Impl;
    explicit DurableStateStore(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace roc::vehicle
