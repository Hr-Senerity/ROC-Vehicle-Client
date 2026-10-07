#include "roc/vehicle/task_protocol.hpp"
#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include "contract_validation.hpp"

#include <jsoncons/json.hpp>

#include <limits>
#include <algorithm>
#include <map>
#include <cmath>
#include <set>
#include <string>
#include <type_traits>

namespace roc::vehicle {
namespace {

using Json = jsoncons::json;

template <typename T>
Result<T> protocol_error(std::string message) {
    if constexpr (std::is_same_v<T,ArtifactManifest>) {
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    }
    return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                 std::move(message)};
}

bool has_exact_members(const Json& value, const std::set<std::string>& expected) {
    if (!value.is_object() || value.size() != expected.size()) {
        return false;
    }
    for (const auto& member : value.object_range()) {
        if (expected.count(std::string{member.key()}) == 0) {
            return false;
        }
    }
    return true;
}

Result<std::string> required_string(const Json& object, std::string_view key,
                                    std::size_t maximum = 4096) {
    if (!object.is_object() || !object.contains(key) ||
        !object.at(key).is_string()) {
        return protocol_error<std::string>("missing or invalid string field: " +
                                           std::string{key});
    }
    auto result = object.at(key).as<std::string>();
    if (result.empty() || result.size() > maximum) {
        return protocol_error<std::string>("string field is empty or oversized: " +
                                           std::string{key});
    }
    return result;
}

Result<std::int64_t> required_integer(const Json& object, std::string_view key,
                                      std::int64_t minimum,
                                      std::int64_t maximum) {
    if (!object.is_object() || !object.contains(key)) {
        return protocol_error<std::int64_t>("missing integer field: " +
                                            std::string{key});
    }
    const auto& value = object.at(key);
    std::int64_t parsed = 0;
    if (value.is_int64()) {
        parsed = value.as<std::int64_t>();
    } else if (value.is_uint64()) {
        const auto unsigned_value = value.as<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(maximum)) {
            return protocol_error<std::int64_t>("integer field is out of range: " +
                                                std::string{key});
        }
        parsed = static_cast<std::int64_t>(unsigned_value);
    } else {
        return protocol_error<std::int64_t>("invalid integer field: " +
                                            std::string{key});
    }
    if (parsed < minimum || parsed > maximum) {
        return protocol_error<std::int64_t>("integer field is out of range: " +
                                            std::string{key});
    }
    return parsed;
}

Result<bool> required_bool(const Json& object, std::string_view key) {
    if (!object.is_object() || !object.contains(key) || !object.at(key).is_bool()) {
        return protocol_error<bool>("missing or invalid boolean field: " +
                                    std::string{key});
    }
    return object.at(key).as<bool>();
}

bool valid_sha256(std::string_view value) {
    if (value.size() != 64) {
        return false;
    }
    for (const char character : value) {
        if (!((character >= '0' && character <= '9') ||
              (character >= 'a' && character <= 'f'))) {
            return false;
        }
    }
    return true;
}

std::string resource_name(ResourceType type) {
    return type == ResourceType::map ? "map" : "road_network";
}

std::string make_task_target(std::string_view task_id, std::string_view suffix) {
    if (!is_valid_uuid(task_id)) {
        return {};
    }
    return "/api/device/tasks/" + std::string{task_id} + "/" +
           std::string{suffix};
}

}  // namespace

Result<AcceptedTask> parse_accept_response(std::string_view json_text,
                                           std::string_view expected_task_id) {
    if (!is_valid_uuid(expected_task_id) || json_text.empty() ||
        json_text.size() > 1024U * 1024U) {
        return protocol_error<AcceptedTask>("accept response input is invalid");
    }
    try {
        if (!detail::valid_task_contract(json_text, "acceptResponse"))
            return protocol_error<AcceptedTask>("accept response violates the fixed contract");
        const auto root = Json::parse(json_text);
        const auto ok = required_bool(root, "ok");
        const auto replayed = required_bool(root, "replayed");
        const auto lease = required_string(root, "lease_token", 8192);
        const auto expiry = required_integer(
            root, "lease_expires_at", 1, std::numeric_limits<std::int64_t>::max());
        if (!ok || !ok.value() || !replayed || !lease || !expiry ||
            !root.contains("task") || !root.at("task").is_object()) {
            return protocol_error<AcceptedTask>("accept response is incomplete or invalid");
        }
        const auto& task = root.at("task");
        const auto task_id = required_string(task, "id", 36);
        const auto attempt = required_integer(task, "attempt", 1,
                                              std::numeric_limits<std::int32_t>::max());
        const auto max_attempts = required_integer(task, "max_attempts", 1, 10);
        const auto progress = required_integer(task, "progress", 0, 100);
        if (!task_id || task_id.value() != expected_task_id || !attempt ||
            !max_attempts || attempt.value() > max_attempts.value() || !progress) {
            return protocol_error<AcceptedTask>("accept task identity or generation is invalid");
        }
        if (
            (!task.at("state").is_string() ||
             task.at("state").as<std::string>() != "accepted")) {
            return protocol_error<AcceptedTask>("accept task state is invalid");
        }
        return AcceptedTask{replayed.value(), lease.value(), expiry.value(),
                            task_id.value(), attempt.value(), max_attempts.value(),
                            progress.value(), task.at("batch_id").as<std::string>(),
                            task.at("vehicle_id").as<std::string>()};
    } catch (const std::exception&) {
        return protocol_error<AcceptedTask>("accept response JSON is invalid");
    }
}

Result<ArtifactManifest> parse_manifest_response(
    std::string_view json_text,
    const TaskAvailableMessage& notification,
    const ArtifactLimits& limits) {
    if (json_text.empty() || json_text.size() > 1024U * 1024U ||
        !is_valid_uuid(notification.task_id)) {
        return protocol_error<ArtifactManifest>("manifest response input is invalid");
    }
    try {
        const auto root = Json::parse(json_text);
        if (root.is_object() && root.contains("manifest") && root.at("manifest").is_object()) {
            const auto& item=root.at("manifest");
            if (item.contains("package_version") &&
                (!(item.at("package_version").is_int64() || item.at("package_version").is_uint64()) ||
                 item.at("package_version").as<std::uint64_t>()!=2))
                return make_resource_failure(ResourceFailure::unsupported_package_version);
            if (item.contains("package_version") &&
                (!item.contains("files") || (item.at("files").is_array() && item.at("files").empty())))
                return make_resource_failure(ResourceFailure::artifact_file_missing);
            if (item.contains("schema_version") &&
                (!(item.at("schema_version").is_int64() || item.at("schema_version").is_uint64()) ||
                 (item.at("schema_version").as<std::uint64_t>()!=1 &&
                  item.at("schema_version").as<std::uint64_t>()!=2)))
                return make_resource_failure(ResourceFailure::unsupported_schema_version);
        }
        if (!detail::valid_task_contract(json_text, "manifestResponse"))
            return protocol_error<ArtifactManifest>("manifest violates the fixed contract");
        static const std::set<std::string> root_fields{"ok", "manifest"};
        if (!has_exact_members(root, root_fields)) {
            return protocol_error<ArtifactManifest>("manifest response fields are invalid");
        }
        const auto ok = required_bool(root, "ok");
        if (!ok || !ok.value() || !root.at("manifest").is_object()) {
            return protocol_error<ArtifactManifest>("manifest response is invalid");
        }
        const auto& manifest = root.at("manifest");
        static const std::set<std::string> required_fields{
            "task_id", "batch_id", "project_id", "resource_type",
            "resource_revision_id", "map_id", "content_type", "byte_size",
            "sha256", "version", "artifact_url"};
        static const std::set<std::string> road_fields{
            "task_id", "batch_id", "project_id", "resource_type",
            "resource_revision_id", "map_id", "content_type", "byte_size",
            "sha256", "version", "schema_version", "artifact_url"};
        const bool has_schema = manifest.contains("schema_version");
        const bool package = manifest.contains("package_version");
        if (!package && !has_exact_members(manifest, has_schema ? road_fields : required_fields)) {
            return protocol_error<ArtifactManifest>("manifest fields are invalid");
        }
        const auto task_id = required_string(manifest, "task_id", 36);
        const auto batch_id = required_string(manifest, "batch_id", 36);
        const auto project_id = required_string(manifest, "project_id", 36);
        const auto revision_id = required_string(manifest, "resource_revision_id", 36);
        const auto map_id = required_string(manifest, "map_id", 36);
        const auto type = required_string(manifest, "resource_type", 32);
        const auto content_type = required_string(manifest, "content_type", 96);
        const auto sha256 = required_string(manifest, "sha256", 64);
        const auto artifact_url = package ? Result<std::string>{std::string{}} :
            required_string(manifest, "artifact_url", 256);
        const auto byte_size = required_integer(
            manifest, "byte_size", 1, std::numeric_limits<std::int64_t>::max());
        const auto version = required_integer(
            manifest, "version", 1, std::numeric_limits<std::int64_t>::max());
        if (!task_id || !batch_id || !project_id || !revision_id || !map_id ||
            !type || !content_type || !sha256 || !artifact_url || !byte_size ||
            !version || !is_valid_uuid(task_id.value()) ||
            !is_valid_uuid(batch_id.value()) || !is_valid_uuid(project_id.value()) ||
            !is_valid_uuid(revision_id.value()) || !is_valid_uuid(map_id.value()) ||
            !valid_sha256(sha256.value())) {
            return protocol_error<ArtifactManifest>("manifest values are invalid");
        }
        const auto expected_url = task_artifact_target(notification.task_id);
        if (task_id.value() != notification.task_id ||
            batch_id.value() != notification.batch_id ||
            project_id.value() != notification.project_id ||
            revision_id.value() != notification.resource_revision_id ||
            type.value() != resource_name(notification.resource_type) ||
            (!package && artifact_url.value() != expected_url)) {
            return protocol_error<ArtifactManifest>("manifest does not match the task notification");
        }
        const auto size = static_cast<std::uint64_t>(byte_size.value());
        const auto maximum = notification.resource_type == ResourceType::map
                                 ? limits.maximum_map_bytes
                                 : limits.maximum_road_network_bytes;
        if (size > maximum) {
            return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
        }
        std::int64_t schema_version = 0;
        ArtifactManifest result;
        if (package) {
            if (notification.resource_type != ResourceType::map || has_schema ||
                manifest.contains("artifact_url"))
                return protocol_error<ArtifactManifest>("package resource fields are invalid");
            result.package_version = 2;
            result.package_digest_algorithm = manifest.at("package_digest_algorithm").as<std::string>();
            result.map_format = manifest.at("map_format").as<std::string>();
            result.coordinate_mode = manifest.at("coordinate_mode").as<std::string>();
            const auto number = [&](const Json& object, const char* name) -> std::optional<double> {
                if (!object.contains(name) || object.at(name).is_null()) return {};
                return object.at(name).as<double>();
            };
            result.resolution = number(manifest, "resolution");
            result.origin_x = number(manifest.at("origin"), "x");
            result.origin_y = number(manifest.at("origin"), "y");
            result.origin_theta = number(manifest.at("origin"), "theta");
            for (const auto* field : {"image_width", "image_height"}) {
                if (manifest.contains(field) && !manifest.at(field).is_null()) {
                    if (std::string_view{field} == "image_width")
                        result.image_width = manifest.at(field).as<std::int64_t>();
                    else result.image_height = manifest.at(field).as<std::int64_t>();
                }
            }
            for (const auto value : {result.resolution, result.origin_x, result.origin_y, result.origin_theta})
                if (value && !std::isfinite(*value))
                    return protocol_error<ArtifactManifest>("package metadata must be finite");
            std::set<std::string> ids, roles;
            std::uint64_t total = 0;
            for (const auto& file : manifest.at("files").array_range()) {
                ArtifactFile item{file.at("id").as<std::string>(), file.at("role").as<std::string>(),
                    file.at("file_name").as<std::string>(), file.at("content_type").as<std::string>(),
                    file.at("byte_size").as<std::uint64_t>(), file.at("sha256").as<std::string>(),
                    file.at("artifact_url").as<std::string>()};
                if (!ids.insert(item.id).second || !roles.insert(item.role).second ||
                    item.artifact_url != task_artifact_file_target(notification.task_id, item.id) ||
                    item.content_type.empty() || item.byte_size > size - total)
                    return protocol_error<ArtifactManifest>("package files are inconsistent");
                total += item.byte_size;
                result.files.push_back(std::move(item));
            }
            const auto expected_roles = result.map_format == "pgm-yaml" ?
                std::set<std::string>{"pgm", "yaml"} : std::set<std::string>{"image"};
            if (roles != expected_roles && std::includes(expected_roles.begin(),expected_roles.end(),
                roles.begin(),roles.end()))
                return make_resource_failure(ResourceFailure::artifact_file_missing);
            if (total != size || roles != expected_roles)
                return protocol_error<ArtifactManifest>("package role set or total size is invalid");
            if (result.map_format != "pgm-yaml" &&
                result.files.front().content_type != (result.map_format == "png" ? "image/png" : "image/jpeg"))
                return protocol_error<ArtifactManifest>("package image MIME is inconsistent");
            const auto digest = map_package_digest(result.files);
            if (!digest || digest.value() != sha256.value())
                return make_resource_failure(ResourceFailure::artifact_hash_mismatch);
        }
        if (notification.resource_type == ResourceType::map) {
            if (!package && (has_schema || (content_type.value() != "image/png" &&
                               content_type.value() != "image/jpeg"))) {
                return protocol_error<ArtifactManifest>("map manifest type is invalid");
            }
        } else {
            const auto schema = required_integer(manifest, "schema_version", 1,
                                                 std::numeric_limits<std::int64_t>::max());
            if (!schema || (schema.value() != 1 && schema.value() != 2) ||
                content_type.value() != (schema.value() == 1 ?
                    "application/vnd.roc.road-network+json" : "application/vnd.roc.road-network.v2+json")) {
                return protocol_error<ArtifactManifest>("road-network manifest is invalid");
            }
            schema_version = schema.value();
        }
        ArtifactManifest base;
        base.task_id = task_id.value();
        base.batch_id = batch_id.value();
        base.project_id = project_id.value();
        base.resource_type = notification.resource_type;
        base.resource_revision_id = revision_id.value();
        base.map_id = map_id.value();
        base.content_type = content_type.value();
        base.byte_size = size;
        base.sha256 = sha256.value();
        base.version = version.value();
        base.schema_version = schema_version;
        base.artifact_url = artifact_url.value();
        base.package_version = result.package_version;
        base.package_digest_algorithm = std::move(result.package_digest_algorithm);
        base.map_format = std::move(result.map_format);
        base.coordinate_mode = std::move(result.coordinate_mode);
        base.image_width = result.image_width;
        base.image_height = result.image_height;
        base.resolution = result.resolution;
        base.origin_x = result.origin_x;
        base.origin_y = result.origin_y;
        base.origin_theta = result.origin_theta;
        base.files = std::move(result.files);
        return base;
    } catch (const std::exception&) {
        return protocol_error<ArtifactManifest>("manifest response JSON is invalid");
    }
}

Result<StatusResponse> parse_status_response(std::string_view json_text,
                                             std::string_view expected_task_id) {
    if (json_text.empty() || json_text.size() > 1024U * 1024U) {
        return protocol_error<StatusResponse>("status response input is invalid");
    }
    try {
        if (!detail::valid_task_contract(json_text, "statusResponse"))
            return protocol_error<StatusResponse>("status response violates the fixed contract");
        const auto root = Json::parse(json_text);
        const auto ok = required_bool(root, "ok");
        const auto replayed = required_bool(root, "replayed");
        if (!ok || !ok.value() || !replayed) {
            return protocol_error<StatusResponse>("status response is invalid");
        }
        const auto& task = root.at("task");
        const auto id = task.at("id").as<std::string>();
        if (!expected_task_id.empty() && id != expected_task_id)
            return protocol_error<StatusResponse>("status response task identity differs");
        return StatusResponse{replayed.value(), root.at("project_id").as<std::string>(),
            id, task.at("state").as<std::string>(), task.at("attempt").as<std::int64_t>(),
            task.at("progress").as<std::int64_t>()};
    } catch (const std::exception&) {
        return protocol_error<StatusResponse>("status response JSON is invalid");
    }
}

Result<HttpErrorPayload> parse_http_error_response(std::string_view json_text,
                                                  unsigned http_status,
                                                  bool artifact_endpoint) {
    if (json_text.empty() || json_text.size() > 1024U * 1024U) {
        return protocol_error<HttpErrorPayload>("HTTP error response input is invalid");
    }
    try {
        const std::map<unsigned, std::string> definitions{
            {400, "deviceBadRequestError"}, {401, "deviceUnauthorizedError"},
            {404, "deviceNotFoundError"}, {409, "deviceConflictError"},
            {416, "deviceRangeError"}, {500, "deviceServerError"}};
        if (http_status != 0) {
            const auto definition = definitions.find(http_status);
            if (definition == definitions.end() || (http_status == 416 && !artifact_endpoint) ||
                !detail::valid_task_contract(json_text, definition->second))
                return protocol_error<HttpErrorPayload>("HTTP error violates the fixed contract");
        } else if (!detail::valid_task_contract(json_text, "deviceErrorResponse"))
            return protocol_error<HttpErrorPayload>("HTTP error shape violates the fixed contract");
        const auto root = Json::parse(json_text);
        const auto ok = required_bool(root, "ok");
        const auto code = required_string(root, "code", 64);
        const auto message = required_string(root, "message", 512);
        if (!ok || ok.value() || !code || !message) {
            return protocol_error<HttpErrorPayload>("HTTP error response is invalid");
        }
        return HttpErrorPayload{code.value(), message.value()};
    } catch (const std::exception&) {
        return protocol_error<HttpErrorPayload>("HTTP error response JSON is invalid");
    }
}

Result<std::string> make_status_request(const StatusEventRecord& event) {
    if (!is_valid_uuid(event.event_id) || !is_valid_uuid(event.generation.task_id) ||
        event.generation.attempt < 1 || event.progress < 0 || event.progress > 100 ||
        event.error_code.size() > 64 || event.error_message.size() > 512 ||
        (event.state == TaskExecutionState::delivered && event.progress != 100) ||
        (event.state == TaskExecutionState::failed && event.error_code.empty()) ||
        (event.state != TaskExecutionState::downloading &&
         event.state != TaskExecutionState::delivering &&
         event.state != TaskExecutionState::delivered &&
         event.state != TaskExecutionState::failed)) {
        return protocol_error<std::string>("status request is invalid");
    }
    std::string state;
    switch (event.state) {
        case TaskExecutionState::downloading: state = "downloading"; break;
        case TaskExecutionState::delivering: state = "delivering"; break;
        case TaskExecutionState::delivered: state = "delivered"; break;
        case TaskExecutionState::failed: state = "failed"; break;
        default: break;
    }
    Json root(jsoncons::json_object_arg);
    root["event_id"] = event.event_id;
    root["state"] = state;
    root["progress"] = event.progress;
    if (!event.error_code.empty()) {
        root["error_code"] = event.error_code;
    }
    if (!event.error_message.empty()) {
        root["error_message"] = event.error_message;
    }
    return root.to_string();
}

std::string task_accept_target(std::string_view task_id) {
    return make_task_target(task_id, "accept");
}

std::string task_manifest_target(std::string_view task_id) {
    return make_task_target(task_id, "manifest");
}

std::string task_artifact_target(std::string_view task_id) {
    return make_task_target(task_id, "artifact");
}

std::string task_status_target(std::string_view task_id) {
    return make_task_target(task_id, "status");
}

std::string task_artifact_file_target(std::string_view task_id, std::string_view file_id) {
    if (!is_valid_uuid(file_id)) return {};
    return make_task_target(task_id, "artifact/" + std::string{file_id});
}

Result<std::string> map_package_digest(const std::vector<ArtifactFile>& files) {
    std::map<std::string, std::string> digests;
    for (const auto& file : files) {
        if (!valid_sha256(file.sha256) || !digests.emplace(file.role, file.sha256).second)
            return protocol_error<std::string>("package digest inputs are invalid");
    }
    std::string input;
    for (const auto& entry : digests) {
        if (!input.empty()) input += '\n';
        input += entry.first + ":" + entry.second;
    }
    return sha256_hex(input);
}

}  // namespace roc::vehicle
