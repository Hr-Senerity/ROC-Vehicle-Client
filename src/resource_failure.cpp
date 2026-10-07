#include "roc/vehicle/resource_failure.hpp"
namespace roc::vehicle {
const char* resource_failure_code(ResourceFailure failure) noexcept {
    switch (failure) {
        case ResourceFailure::unsupported_package_version: return "unsupported_package_version";
        case ResourceFailure::unsupported_schema_version: return "unsupported_schema_version";
        case ResourceFailure::artifact_file_missing: return "artifact_file_missing";
        case ResourceFailure::artifact_hash_mismatch: return "artifact_hash_mismatch";
        case ResourceFailure::artifact_metadata_mismatch: return "artifact_metadata_mismatch";
        case ResourceFailure::road_network_invalid: return "road_network_invalid";
        case ResourceFailure::local_storage_failure: return "local_storage_failure";
        case ResourceFailure::receiver_commit_failed: return "receiver_commit_failed";
    }
    return "artifact_metadata_mismatch";
}
Error make_resource_failure(ResourceFailure failure) {
    const bool storage = failure == ResourceFailure::local_storage_failure;
    const bool unsupported = failure == ResourceFailure::unsupported_package_version ||
                             failure == ResourceFailure::unsupported_schema_version;
    return {storage ? ErrorDomain::storage : ErrorDomain::artifact,
            storage ? ErrorCode::io_failure :
                (unsupported ? ErrorCode::unsupported : ErrorCode::integrity_failure),
            false, "resource operation failed", resource_failure_code(failure)};
}
std::optional<ResourceFailure> classify_resource_failure(const Error& error, bool artifact_endpoint) {
    for (auto value : {ResourceFailure::unsupported_package_version,
                      ResourceFailure::unsupported_schema_version, ResourceFailure::artifact_file_missing,
                      ResourceFailure::artifact_hash_mismatch, ResourceFailure::artifact_metadata_mismatch,
                      ResourceFailure::road_network_invalid, ResourceFailure::local_storage_failure,
                      ResourceFailure::receiver_commit_failed}) {
        if (error.server_code == resource_failure_code(value)) return value;
    }
    if (artifact_endpoint && error.domain == ErrorDomain::http &&
        error.server_code == "artifact_not_found") return ResourceFailure::artifact_file_missing;
    if (error.domain == ErrorDomain::storage && error.code == ErrorCode::io_failure)
        return ResourceFailure::local_storage_failure;
    return {};
}
}
