#pragma once
#include "roc/vehicle/artifact_receiver.hpp"
#include "roc/vehicle/package_sink.hpp"

namespace roc::vehicle {
using VerifiedPackageCallback = std::function<Result<void>(const DeliveredMapPackage&)>;
class PackageStreamTransaction {
public:
    virtual ~PackageStreamTransaction() = default;
    // begin receives all immutable metadata; IDs distinguish original file byte streams.
    [[nodiscard]] virtual Result<void> write(std::string_view file_id,
        const std::uint8_t* data, std::size_t size) = 0;
    [[nodiscard]] virtual Result<void> commit() = 0;
    virtual void abort() noexcept = 0;
};
class PackageStreamReceiver {
public:
    virtual ~PackageStreamReceiver() = default;
    [[nodiscard]] virtual Result<std::unique_ptr<PackageStreamTransaction>> begin(
        const DeliveredMapPackage& package) = 0;
};
[[nodiscard]] Result<DeliveryReceipt> deliver_package_to_verified_callback(
    const DeliveredMapPackage& package, const VerifiedPackageCallback& callback,
    const CancellationSignal* cancellation = nullptr);
[[nodiscard]] Result<DeliveryReceipt> deliver_package_to_transactional_stream(
    const DeliveredMapPackage& package, PackageStreamReceiver& receiver,
    std::size_t chunk_size = 64U*1024U, const CancellationSignal* cancellation = nullptr);
}
