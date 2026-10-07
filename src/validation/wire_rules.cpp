#include "wire_rules.hpp"
#include "../contract_validation.hpp"
#include "roc/vehicle/protocol.hpp"
#include <jsoncons_ext/jsonschema/common/format_validators.hpp>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <regex>

namespace roc::vehicle::detail::wire {
namespace {
using Names = std::initializer_list<const char*>;
bool fields(const Json& j, Names required, Names optional = {}, bool extensions = false) {
    if (!j.is_object()) return false;
    for (const auto* key : required) if (!j.contains(key)) return false;
    if (extensions) return true;
    for (const auto& member : j.object_range()) {
        const auto known = [&](Names names) {
            return std::any_of(names.begin(), names.end(), [&](const char* name) { return member.key() == name; });
        };
        if (!known(required) && !known(optional)) return false;
    }
    return true;
}
bool text(const Json& j, std::size_t low = 0, std::size_t high = std::numeric_limits<std::size_t>::max()) {
    if (!j.is_string()) return false;
    const auto s = j.as<std::string>();
    // String limits count Unicode scalar values rather than UTF-8 bytes.
    const auto size = static_cast<std::size_t>(std::count_if(s.begin(), s.end(), [](unsigned char c) { return (c & 0xc0U) != 0x80U; }));
    return size >= low && size <= high;
}
bool one(const Json& j, Names values) {
    if (!j.is_string()) return false;
    const auto s = j.as<std::string>();
    return std::any_of(values.begin(), values.end(), [&](const char* value) { return s == value; });
}
bool numeric(const Json& j, long double low = -std::numeric_limits<long double>::infinity(),
             long double high = std::numeric_limits<long double>::infinity(), bool integral = false, bool exclusive = false) {
    if (!j.is_number()) return false;
    const auto n = j.as<long double>();
    return std::isfinite(n) && (exclusive ? n > low : n >= low) && n <= high && (!integral || std::floor(n) == n);
}
bool integer(const Json& j, long double low = 0, long double high = std::numeric_limits<long double>::infinity()) {
    return numeric(j, low, high, true);
}
bool constant(const Json& j, long double n) { return numeric(j, n, n); }
bool uuid(const Json& j) { return j.is_string() && is_valid_uuid(j.as<std::string>()); }
bool time(const Json& j) {
    if (!j.is_string()) return false;
    const auto s = j.as<std::string>();
    return jsoncons::jsonschema::validate_date_time_rfc3339(s, jsoncons::jsonschema::date_time_type::date_time);
}
bool sequence(const Json& j, bool zero = false) {
    if (!j.is_string()) return false;
    const auto s = j.as<std::string>();
    // parse_nonnegative_cursor also consumes the complete string and checks INT64.
    if (s.empty() || s.front() < '0' || s.front() > '9') return false;
    return zero ? static_cast<bool>(parse_nonnegative_cursor(s)) : static_cast<bool>(parse_positive_sequence(s));
}
bool pattern(const Json& j, const std::regex& p) { return j.is_string() && std::regex_match(j.as<std::string>(), p); }
bool timestamp_pattern(const Json& j) {
    static const std::regex p{R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]{1,9})?(Z|[+-][0-9]{2}:[0-9]{2})$)"};
    return pattern(j, p);
}
bool library(const Json& j) {
    static const std::regex p{R"(^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$)"};
    return pattern(j, p);
}
bool optional_uuid(const Json& j, const char* name) { return !j.contains(name) || uuid(j.at(name)); }
bool nullable_text(const Json& j, std::size_t high) { return j.is_null() || text(j, 0, high); }
bool speed(const Json& j) { return j.is_null() || numeric(j, 0, 100, false, true); }
bool mode(const Json& j) { return one(j, {"metric", "legacy-normalized"}); }
bool resource(const Json& j) { return one(j, {"map", "road_network"}); }
bool id(const Json& j, std::size_t maximum = 64) {
    if (!text(j, 1, maximum)) return false;
    const auto s = j.as<std::string>();
    const auto alnum = [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    return alnum(s.front()) && std::all_of(s.begin(), s.end(), [&](char c) { return alnum(c) || c == '.' || c == '_' || c == ':' || c == '-'; });
}
bool array(const Json& j, std::size_t maximum, bool (*item)(const Json&), std::size_t minimum = 0) {
    if (!j.is_array() || j.size() < minimum || j.size() > maximum) return false;
    for (const auto& value : j.array_range()) if (!item(value)) return false;
    return true;
}
bool envelope(const Json& j, bool task_timestamp = false) {
    return fields(j, {"protocol_version", "message_id", "type", "sequence", "timestamp", "payload"}) &&
        constant(j.at("protocol_version"), 1) && uuid(j.at("message_id")) && text(j.at("type")) &&
        sequence(j.at("sequence")) && (task_timestamp ? time(j.at("timestamp")) : timestamp_pattern(j.at("timestamp"))) && j.at("payload").is_object();
}
bool error_payload(const Json& p) {
    return fields(p, {"code", "message"}, {"request_message_id"}, true) &&
        one(p.at("code"), {"authentication_failed", "authentication_required", "capacity_reached", "connection_replaced",
            "credential_revoked", "credential_rotated", "heartbeat_timeout", "identity_not_allowed", "invalid_json",
            "invalid_library_version", "invalid_message_id", "invalid_message_type", "invalid_payload", "invalid_sequence",
            "invalid_telemetry", "invalid_timestamp", "invalid_type", "message_rejected", "message_too_large", "service_unavailable",
            "text_required", "unknown_field", "unsupported_message", "unsupported_protocol_version"}) &&
        text(p.at("message"), 1, 512) && optional_uuid(p, "request_message_id");
}
bool heartbeat(const Json& p) { return fields(p, {}, {"library_version"}) && (!p.contains("library_version") || library(p.at("library_version"))); }
bool telemetry(const Json& p) {
    if (!fields(p, {"online", "cpu_usage", "memory_usage", "battery_level", "localization_confidence", "position", "velocity"}, {"library_version"}) ||
        !p.at("online").is_bool() || !numeric(p.at("cpu_usage"), 0, 100) || !numeric(p.at("memory_usage"), 0, 100) ||
        !integer(p.at("battery_level"), 0, 100) || !numeric(p.at("localization_confidence"), 0, 100) ||
        (p.contains("library_version") && !library(p.at("library_version")))) return false;
    const auto& pos = p.at("position"); const auto& v = p.at("velocity");
    return fields(pos, {"x", "y", "theta"}) && numeric(pos.at("x")) && numeric(pos.at("y")) && numeric(pos.at("theta")) &&
        fields(v, {"linear", "angular"}) && numeric(v.at("linear")) && numeric(v.at("angular"));
}
bool task_state(const Json& j) { return one(j, {"queued", "offered", "accepted", "downloading", "delivering", "delivered", "failed", "canceled", "expired"}); }
bool task_object(const Json& j) {
    if (!fields(j, {"id", "batch_id", "vehicle_id", "state", "attempt", "max_attempts", "progress", "error_code", "error_message",
                    "offered_at", "accepted_at", "delivered_at", "updated_at"}, {}, true) ||
        !uuid(j.at("id")) || !uuid(j.at("batch_id")) || !uuid(j.at("vehicle_id")) || !task_state(j.at("state")) ||
        !integer(j.at("attempt")) || !integer(j.at("max_attempts"), 1, 10) || !integer(j.at("progress"), 0, 100) ||
        !nullable_text(j.at("error_code"), 64) || !nullable_text(j.at("error_message"), 512) || !time(j.at("updated_at"))) return false;
    for (const auto* key : {"offered_at", "accepted_at", "delivered_at"}) if (!j.at(key).is_null() && !time(j.at(key))) return false;
    return true;
}
bool digest(const Json& j) {
    if (!text(j, 64, 64)) return false;
    const auto s = j.as<std::string>();
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool artifact_url(const Json& j, bool file) {
    static const std::regex single{R"(^/api/device/tasks/[0-9a-f-]+/artifact$)"};
    static const std::regex multiple{R"(^/api/device/tasks/[0-9a-f-]+/artifact/[0-9a-f-]+$)"};
    return pattern(j, file ? multiple : single);
}
bool manifest_file(const Json& j) {
    return fields(j, {"id", "role", "file_name", "content_type", "byte_size", "sha256", "artifact_url"}) && uuid(j.at("id")) &&
        one(j.at("role"), {"image", "pgm", "yaml"}) && text(j.at("file_name"), 1, 255) && text(j.at("content_type"), 0, 96) &&
        integer(j.at("byte_size"), 1) && digest(j.at("sha256")) && artifact_url(j.at("artifact_url"), true);
}
bool manifest(const Json& j) {
    if (!fields(j, {"task_id", "batch_id", "project_id", "resource_type", "resource_revision_id", "map_id", "content_type", "byte_size", "sha256", "version"},
        {"schema_version", "package_version", "package_digest_algorithm", "map_format", "coordinate_mode", "image_width", "image_height", "resolution", "origin", "files", "artifact_url"})) return false;
    for (const auto* key : {"task_id", "batch_id", "project_id", "resource_revision_id", "map_id"}) if (!uuid(j.at(key))) return false;
    if (!resource(j.at("resource_type")) || !text(j.at("content_type"), 0, 96) || !integer(j.at("byte_size"), 1) ||
        !digest(j.at("sha256")) || !integer(j.at("version"), 1)) return false;
    if (j.contains("schema_version") && !integer(j.at("schema_version"), 1, 2)) return false;
    if (j.contains("package_version") && !constant(j.at("package_version"), 2)) return false;
    if (j.contains("package_digest_algorithm") && !one(j.at("package_digest_algorithm"), {"roc-file-set-v1"})) return false;
    if (j.contains("map_format") && !one(j.at("map_format"), {"png", "jpeg", "pgm-yaml"})) return false;
    if (j.contains("coordinate_mode") && !mode(j.at("coordinate_mode"))) return false;
    for (const auto* key : {"image_width", "image_height"}) if (j.contains(key) && !j.at(key).is_null() && !integer(j.at(key), 1)) return false;
    if (j.contains("resolution") && !j.at("resolution").is_null() && !numeric(j.at("resolution"), 0, std::numeric_limits<long double>::infinity(), false, true)) return false;
    if (j.contains("origin")) {
        const auto& origin = j.at("origin");
        if (!fields(origin, {"x", "y", "theta"})) return false;
        for (const auto* key : {"x", "y", "theta"}) if (!origin.at(key).is_null() && !numeric(origin.at(key))) return false;
    }
    if (j.contains("files") && !array(j.at("files"), 3, manifest_file, 1)) return false;
    if (j.contains("artifact_url") && !artifact_url(j.at("artifact_url"), false)) return false;
    if (j.contains("package_version")) {
        for (const auto* key : {"package_digest_algorithm", "map_format", "coordinate_mode", "origin", "files"}) if (!j.contains(key)) return false;
    } else if (!j.contains("artifact_url")) return false;
    return true;
}
bool point(const Json& j) { return fields(j, {"x", "y"}) && numeric(j.at("x")) && numeric(j.at("y")); }
bool node(const Json& j) {
    return fields(j, {"id", "x", "y", "kind", "label"}) && id(j.at("id")) && numeric(j.at("x")) && numeric(j.at("y")) &&
        one(j.at("kind"), {"waypoint"}) && text(j.at("label"), 0, 128);
}
bool geometry(const Json& j) {
    if (!j.is_object() || !j.contains("type")) return false;
    if (one(j.at("type"), {"line"})) return fields(j, {"type"});
    return fields(j, {"type", "control1", "control2"}) && one(j.at("type"), {"cubic_bezier"}) && point(j.at("control1")) && point(j.at("control2"));
}
bool edge1(const Json& j) {
    return fields(j, {"id", "from", "to", "direction"}, {"max_speed_mps"}) && id(j.at("id")) && id(j.at("from")) && id(j.at("to")) &&
        one(j.at("direction"), {"both", "forward"}) && (!j.contains("max_speed_mps") || speed(j.at("max_speed_mps")));
}
bool edge2(const Json& j) {
    return fields(j, {"id", "from", "to", "direction", "max_speed_mps", "geometry"}) && id(j.at("id")) && id(j.at("from")) && id(j.at("to")) &&
        one(j.at("direction"), {"both", "forward"}) && speed(j.at("max_speed_mps")) && geometry(j.at("geometry"));
}
bool sample(const Json& j) {
    return fields(j, {"index", "x", "y", "s", "heading"}) && integer(j.at("index")) && numeric(j.at("x")) && numeric(j.at("y")) &&
        numeric(j.at("s"), 0) && numeric(j.at("heading"));
}
bool trajectory(const Json& j) {
    return fields(j, {"id", "edge_id", "direction", "max_speed_mps", "points", "length"}) && id(j.at("id"), 80) && id(j.at("edge_id")) &&
        one(j.at("direction"), {"forward", "reverse"}) && speed(j.at("max_speed_mps")) && array(j.at("points"), 10000, sample, 2) && numeric(j.at("length"), 0);
}
}

bool device(const Json& j, std::string_view definition) {
    if (definition == "positiveInt64String") return sequence(j);
    if (definition == "nonNegativeInt64String") return sequence(j, true);
    if (definition == "libraryVersion") return library(j);
    if (definition == "heartbeatPayload") return heartbeat(j);
    if (definition == "telemetryPayload") return telemetry(j);
    if (!envelope(j)) return false;
    if (definition == "serverMessageEnvelope") return true;
    const auto& p = j.at("payload");
    if (definition == "upstream") return (one(j.at("type"), {"heartbeat"}) && heartbeat(p)) || (one(j.at("type"), {"telemetry"}) && telemetry(p));
    if (definition == "hello") {
        return one(j.at("type"), {"hello"}) && fields(p, {"vehicle_id", "heartbeat_interval_seconds", "idle_timeout_seconds",
            "max_message_bytes", "max_heartbeat_bytes", "max_telemetry_bytes", "last_client_sequence"}, {}, true) &&
            uuid(p.at("vehicle_id")) && integer(p.at("heartbeat_interval_seconds"), 1, 3600) && integer(p.at("idle_timeout_seconds"), 2, 7200) &&
            constant(p.at("max_message_bytes"), 65536) && constant(p.at("max_heartbeat_bytes"), 4096) && constant(p.at("max_telemetry_bytes"), 16384) && sequence(p.at("last_client_sequence"), true);
    }
    if (definition == "ack") return one(j.at("type"), {"ack"}) && fields(p, {"ack_message_id", "ack_sequence", "accepted_type", "duplicate"}, {}, true) &&
        uuid(p.at("ack_message_id")) && sequence(p.at("ack_sequence")) && one(p.at("accepted_type"), {"heartbeat", "telemetry"}) && p.at("duplicate").is_bool();
    if (definition == "error") return one(j.at("type"), {"error"}) && error_payload(p);
    return false;
}
bool task(const Json& j, std::string_view definition) {
    if (definition == "uuid") return uuid(j);
    if (definition == "positiveInt64String") return sequence(j);
    if (definition == "resourceType") return resource(j);
    if (definition == "taskState") return task_state(j);
    if (definition == "task") return task_object(j);
    if (definition == "taskAvailable") {
        if (!envelope(j, true) || !one(j.at("type"), {"task.available"})) return false;
        const auto& p = j.at("payload");
        if (!fields(p, {"task_id", "batch_id", "project_id", "resource_type", "resource_revision_id", "attempt", "max_attempts"}, {}, true)) return false;
        for (const auto* key : {"task_id", "batch_id", "project_id", "resource_revision_id"}) if (!uuid(p.at(key))) return false;
        return resource(p.at("resource_type")) && integer(p.at("attempt")) && integer(p.at("max_attempts"), 1, 10);
    }
    if (definition == "statusRequest") {
        if (!fields(j, {"event_id", "state", "progress"}, {"error_code", "error_message"}) || !uuid(j.at("event_id")) ||
            !one(j.at("state"), {"downloading", "delivering", "delivered", "failed"}) || !integer(j.at("progress"), 0, 100) ||
            (j.contains("error_code") && !text(j.at("error_code"), 0, 64)) || (j.contains("error_message") && !text(j.at("error_message"), 0, 512))) return false;
        return (!one(j.at("state"), {"delivered"}) || constant(j.at("progress"), 100)) &&
            (!one(j.at("state"), {"failed"}) || (j.contains("error_code") && text(j.at("error_code"), 1, 64)));
    }
    if (definition == "acceptResponse") return fields(j, {"ok", "replayed", "lease_token", "lease_expires_at", "task"}, {}, true) &&
        j.at("ok").is_bool() && j.at("ok").as<bool>() && j.at("replayed").is_bool() && text(j.at("lease_token"), 16) && integer(j.at("lease_expires_at"), 1) && task_object(j.at("task"));
    if (definition == "statusResponse") return fields(j, {"ok", "replayed", "project_id", "task"}, {}, true) &&
        j.at("ok").is_bool() && j.at("ok").as<bool>() && j.at("replayed").is_bool() && uuid(j.at("project_id")) && task_object(j.at("task"));
    if (definition == "manifestResponse") return fields(j, {"ok", "manifest"}) && j.at("ok").is_bool() && j.at("ok").as<bool>() && manifest(j.at("manifest"));
    if (!fields(j, {"ok", "code", "message"}) || !j.at("ok").is_bool() || j.at("ok").as<bool>() || !text(j.at("code")) || !text(j.at("message"), 1, 512)) return false;
    const auto& code = j.at("code");
    if (definition == "deviceErrorResponse") return true;
    if (definition == "deviceBadRequestError") return one(code, {"invalid_id", "invalid_json", "unknown_field", "invalid_status_state", "invalid_progress", "invalid_error_code", "invalid_error_message", "error_code_required"});
    if (definition == "deviceUnauthorizedError") return one(code, {"authentication_failed", "invalid_lease"});
    if (definition == "deviceNotFoundError") return one(code, {"task_not_found", "artifact_not_found"});
    if (definition == "deviceConflictError") return one(code, {"invalid_state", "lease_expired", "lease_conflict", "attempts_exhausted", "event_conflict", "invalid_transition", "file_id_required"});
    if (definition == "deviceRangeError") return one(code, {"range_not_supported"});
    if (definition == "deviceServerError") return one(code, {"internal_error"});
    return false;
}
bool road(const Json& j, int version) {
    if (version == 1) return fields(j, {"schema_version", "coordinate_mode", "nodes", "edges"}) && constant(j.at("schema_version"), 1) &&
        mode(j.at("coordinate_mode")) && array(j.at("nodes"), 10000, node) && array(j.at("edges"), 50000, edge1);
    if (version != 2 || !fields(j, {"schema_version", "coordinate_mode", "sampling", "nodes", "edges", "trajectories"}) ||
        !constant(j.at("schema_version"), 2) || !mode(j.at("coordinate_mode"))) return false;
    const auto& s = j.at("sampling");
    return fields(s, {"algorithm", "spacing", "precision"}) && one(s.at("algorithm"), {"uniform-parameter-v1"}) && constant(s.at("precision"), 6) &&
        numeric(s.at("spacing"), 0.000001, one(j.at("coordinate_mode"), {"legacy-normalized"}) ? 1 : 100) &&
        array(j.at("nodes"), 10000, node) && array(j.at("edges"), 50000, edge2) && array(j.at("trajectories"), 100000, trajectory);
}
}
