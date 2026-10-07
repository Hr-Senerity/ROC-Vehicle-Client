#pragma once

#include "roc/vehicle/config.hpp"
#include "roc/vehicle/protocol.hpp"
#include "roc/vehicle/result.hpp"
#include "roc/vehicle/state_store.hpp"

#include <cstdint>
#include <optional>
#include <vector>
#include <string>
#include <string_view>

namespace roc::vehicle {

struct AcceptedTask {
    bool replayed{false};
    std::string lease_token;
    std::int64_t lease_expires_at{0};
    std::string task_id;
    std::int64_t attempt{0};
    std::int64_t max_attempts{0};
    std::int64_t progress_floor{0};
    std::string batch_id;
    std::string vehicle_id;

    [[nodiscard]] bool is_expired_at(std::int64_t unix_seconds) const noexcept {
        return unix_seconds > lease_expires_at;
    }
};

struct ArtifactFile {
    std::string id;
    std::string role;
    std::string file_name;
    std::string content_type;
    std::uint64_t byte_size{0};
    std::string sha256;
    std::string artifact_url;
};

struct ArtifactManifest {
    std::string task_id;
    std::string batch_id;
    std::string project_id;
    ResourceType resource_type{ResourceType::map};
    std::string resource_revision_id;
    std::string map_id;
    std::string content_type;
    std::uint64_t byte_size{0};
    std::string sha256;
    std::int64_t version{0};
    std::int64_t schema_version{0};
    std::string artifact_url;
    std::int64_t package_version{0}; // absent on the wire for legacy maps
    std::string package_digest_algorithm;
    std::string map_format;
    std::string coordinate_mode;
    std::optional<std::int64_t> image_width;
    std::optional<std::int64_t> image_height;
    std::optional<double> resolution;
    std::optional<double> origin_x;
    std::optional<double> origin_y;
    std::optional<double> origin_theta;
    std::vector<ArtifactFile> files;
};

struct StatusResponse {
    bool replayed{false};
    std::string project_id;
    std::string task_id;
    std::string state;
    std::int64_t attempt{0};
    std::int64_t progress{0};
};

struct HttpErrorPayload {
    std::string code;
    std::string message;
};

[[nodiscard]] Result<AcceptedTask> parse_accept_response(
    std::string_view json_text,
    std::string_view expected_task_id);

[[nodiscard]] Result<ArtifactManifest> parse_manifest_response(
    std::string_view json_text,
    const TaskAvailableMessage& notification,
    const ArtifactLimits& limits);

[[nodiscard]] Result<StatusResponse> parse_status_response(
    std::string_view json_text,
    std::string_view expected_task_id = {});

[[nodiscard]] Result<HttpErrorPayload> parse_http_error_response(
    std::string_view json_text,
    unsigned http_status = 0,
    bool artifact_endpoint = false);

[[nodiscard]] Result<std::string> map_package_digest(const std::vector<ArtifactFile>& files);

[[nodiscard]] Result<std::string> make_status_request(
    const StatusEventRecord& event);

[[nodiscard]] std::string task_accept_target(std::string_view task_id);
[[nodiscard]] std::string task_manifest_target(std::string_view task_id);
[[nodiscard]] std::string task_artifact_target(std::string_view task_id);
[[nodiscard]] std::string task_artifact_file_target(std::string_view task_id,
                                                   std::string_view file_id);
[[nodiscard]] std::string task_status_target(std::string_view task_id);

}  // namespace roc::vehicle
