#pragma once
#include "roc/vehicle/errors.hpp"
#include <optional>

namespace roc::vehicle {
enum class ResourceFailure {
    unsupported_package_version, unsupported_schema_version, artifact_file_missing,
    artifact_hash_mismatch, artifact_metadata_mismatch, road_network_invalid,
    local_storage_failure, receiver_commit_failed
};
[[nodiscard]] const char* resource_failure_code(ResourceFailure failure) noexcept;
// Fixed redacted messages only. Never copy an endpoint, path, lease or callback exception.
[[nodiscard]] Error make_resource_failure(ResourceFailure failure);
// Does not classify transport/lease/auth/unknown responses as resource failure.
[[nodiscard]] std::optional<ResourceFailure> classify_resource_failure(
    const Error& error, bool artifact_endpoint = false);
}
