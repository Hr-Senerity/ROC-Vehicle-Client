#include "roc/vehicle/package_receiver.hpp"
#include "roc/vehicle/resource_failure.hpp"
namespace roc::vehicle {
namespace {
Error failed_receiver() { return make_resource_failure(ResourceFailure::receiver_commit_failed); }
Error canceled_receiver() { return {ErrorDomain::canceled,ErrorCode::canceled,false,"package canceled"}; }
class AbortGuard {
public:
    explicit AbortGuard(PackageStreamTransaction& transaction) : transaction_(transaction) {}
    ~AbortGuard() { if (!committed_) transaction_.abort(); }
    void committed() noexcept { committed_=true; }
private: PackageStreamTransaction& transaction_; bool committed_{false};
};
}
Result<DeliveryReceipt> deliver_package_to_verified_callback(const DeliveredMapPackage& package,
    const VerifiedPackageCallback& callback, const CancellationSignal* cancel) {
    if (!callback) return failed_receiver();
    if (cancel && cancel->is_canceled()) return canceled_receiver();
    const auto valid=verify_map_package(package); if (!valid) return valid.error();
    if (cancel && cancel->is_canceled()) return canceled_receiver();
    try {
        if (!callback(package)) return failed_receiver();
    } catch (...) { return failed_receiver(); }
    const auto unchanged=verify_map_package(package); if (!unchanged) return unchanged.error();
    return DeliveryReceipt{true};
}
Result<DeliveryReceipt> deliver_package_to_transactional_stream(const DeliveredMapPackage& package,
    PackageStreamReceiver& receiver, std::size_t chunk_size, const CancellationSignal* cancel) {
    if (chunk_size==0 || chunk_size>1024U*1024U) return failed_receiver();
    if (cancel && cancel->is_canceled()) return canceled_receiver();
    const auto valid=verify_map_package(package); if (!valid) return valid.error();
    try {
        auto opened=receiver.begin(package);
        if (!opened || !opened.value()) return failed_receiver();
        auto& transaction=*opened.value(); AbortGuard guard(transaction);
        const auto streamed=stream_map_package(package,
            [&](std::string_view id,const std::uint8_t* data,std::size_t size) -> Result<void> {
                if (!transaction.write(id,data,size)) return failed_receiver();
                return {};
            },chunk_size,cancel);
        if (!streamed) return streamed.error();
        const auto unchanged=verify_map_package(package); if (!unchanged) return unchanged.error();
        if (cancel && cancel->is_canceled()) return canceled_receiver();
        if (!transaction.commit()) return failed_receiver();
        guard.committed(); return DeliveryReceipt{true};
    } catch (...) { return failed_receiver(); }
}
}
