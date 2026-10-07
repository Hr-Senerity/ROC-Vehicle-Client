#pragma once

#include "roc/vehicle/config.hpp"
#include "roc/vehicle/result.hpp"
#include "roc/vehicle/state_store.hpp"
#include "roc/vehicle/task_protocol.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace roc::vehicle {

class CancellationSignal {
public:
    void cancel() noexcept { canceled_.store(true, std::memory_order_release); }
    [[nodiscard]] bool is_canceled() const noexcept {
        return canceled_.load(std::memory_order_acquire);
    }

private:
    std::atomic_bool canceled_{false};
};

struct ArtifactHttpMetadata {
    std::string content_type;
    std::string sha256;
    std::optional<std::uint64_t> content_length;
    std::uint64_t bytes_received{0};
};

using ArtifactChunkHandler =
    std::function<Result<void>(const std::uint8_t*, std::size_t)>;

// Blocking task HTTP operations. Call these from an application worker, never
// from a DeviceClient callback or its I/O thread. Each operation obtains a
// fresh token and uses a new same-origin connection.
// Socket connect/handshake/read/write use config.http_operation_timeout.
// DNS resolution and application callbacks are not covered by that deadline.
class DeviceTaskHttpClient {
public:
    static Result<std::unique_ptr<DeviceTaskHttpClient>> create(
        ClientConfig config,
        DeviceTokenProvider token_provider);

    DeviceTaskHttpClient(const DeviceTaskHttpClient&) = delete;
    DeviceTaskHttpClient& operator=(const DeviceTaskHttpClient&) = delete;
    DeviceTaskHttpClient(DeviceTaskHttpClient&&) = delete;
    DeviceTaskHttpClient& operator=(DeviceTaskHttpClient&&) = delete;
    ~DeviceTaskHttpClient();

    [[nodiscard]] Result<AcceptedTask> accept_task(
        const TaskAvailableMessage& notification,
        DurableStateStore& state_store,
        const CancellationSignal* cancellation = nullptr) const;

    [[nodiscard]] Result<ArtifactManifest> read_manifest(
        const TaskAvailableMessage& notification,
        const AcceptedTask& accepted,
        DurableStateStore& state_store,
        const CancellationSignal* cancellation = nullptr) const;

    [[nodiscard]] Result<ArtifactHttpMetadata> download_artifact(
        const ArtifactManifest& manifest,
        const AcceptedTask& accepted,
        DurableStateStore& state_store,
        ArtifactChunkHandler on_chunk,
        const CancellationSignal* cancellation = nullptr) const;

    [[nodiscard]] Result<ArtifactHttpMetadata> download_artifact_file(
        const ArtifactManifest& package,
        std::string_view file_id,
        const AcceptedTask& accepted,
        DurableStateStore& state_store,
        ArtifactChunkHandler on_chunk,
        const CancellationSignal* cancellation = nullptr) const;

    // Local recovery only; never sends accept. Replay pending events first.
    [[nodiscard]] Result<AcceptedTask> recover_accepted_task(
        const TaskGenerationKey& key, DurableStateStore& state_store) const;

    [[nodiscard]] Result<StatusResponse> report_status(
        const StatusEventRecord& event,
        const AcceptedTask& accepted,
        DurableStateStore& state_store,
        const CancellationSignal* cancellation = nullptr) const;

    // Replays the exact persisted request and lease. Terminal events are sent
    // even after the local lease deadline so SYSTEM can return replayed:true.
    [[nodiscard]] Result<StatusResponse> replay_status_event(
        const StatusEventRecord& event,
        DurableStateStore& state_store,
        const CancellationSignal* cancellation = nullptr) const;

private:
    struct Impl;
    explicit DeviceTaskHttpClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace roc::vehicle
