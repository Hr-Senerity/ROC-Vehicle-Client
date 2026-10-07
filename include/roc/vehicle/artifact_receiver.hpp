#pragma once

#include "roc/vehicle/file_sink.hpp"
#include "roc/vehicle/result.hpp"
#include "roc/vehicle/task_http_client.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace roc::vehicle {

struct DeliveryReceipt {
    bool committed{false};
};

using VerifiedFileCallback =
    std::function<Result<void>(const DeliveredArtifact&)>;

class ArtifactStreamTransaction {
public:
    virtual ~ArtifactStreamTransaction() = default;
    [[nodiscard]] virtual Result<void> write(const std::uint8_t* data,
                                             std::size_t size) = 0;
    [[nodiscard]] virtual Result<void> commit() = 0;
    virtual void abort() noexcept = 0;
};

class TransactionalStreamReceiver {
public:
    virtual ~TransactionalStreamReceiver() = default;
    [[nodiscard]] virtual Result<std::unique_ptr<ArtifactStreamTransaction>> begin(
        const DeliveredArtifact& artifact) = 0;
};

[[nodiscard]] Result<DeliveryReceipt> deliver_to_verified_file_callback(
    const DeliveredArtifact& artifact,
    const VerifiedFileCallback& callback,
    const CancellationSignal* cancellation = nullptr);

[[nodiscard]] Result<DeliveryReceipt> deliver_to_transactional_stream(
    const DeliveredArtifact& artifact,
    TransactionalStreamReceiver& receiver,
    std::size_t chunk_size = 64U * 1024U,
    const CancellationSignal* cancellation = nullptr);

}  // namespace roc::vehicle
