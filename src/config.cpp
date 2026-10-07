#include "roc/vehicle/config.hpp"
#include "roc/vehicle/endpoint.hpp"
#include "roc/vehicle/protocol.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <string_view>

namespace roc::vehicle {
namespace {

Result<void> invalid(std::string message) {
    return Error{ErrorDomain::configuration, ErrorCode::invalid_argument, false,
                 std::move(message)};
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

}  // namespace

Result<void> validate_config(const ClientConfig& config) {
    if (config.base_url.empty()) {
        return invalid("base_url is required");
    }
    if (!starts_with(config.base_url, "http://") &&
        !starts_with(config.base_url, "https://")) {
        return invalid("base_url must use http or https");
    }
    if (config.base_url.find('@') != std::string::npos ||
        config.base_url.find('#') != std::string::npos ||
        config.base_url.find('?') != std::string::npos) {
        return invalid("base_url must not contain credentials, query, or fragment");
    }
    const auto endpoint = derive_device_endpoint(config.base_url);
    if (!endpoint) {
        return endpoint.error();
    }
    if (config.state_directory.empty() || config.artifact_directory.empty()) {
        return invalid("state_directory and artifact_directory are required");
    }
    if (config.task_queue_capacity == 0) {
        return invalid("task_queue_capacity must be positive");
    }
    if (!config.expected_vehicle_id.empty() && !is_valid_uuid(config.expected_vehicle_id)) {
        return invalid("expected_vehicle_id must be a UUID");
    }
    if (config.hello_timeout.count() <= 0) {
        return invalid("hello_timeout must be positive");
    }
    if (config.http_operation_timeout.count() <= 0) {
        return invalid("http_operation_timeout must be positive");
    }
    if (config.reconnect.initial_delay.count() <= 0 ||
        config.reconnect.maximum_delay < config.reconnect.initial_delay) {
        return invalid("reconnect delay range is invalid");
    }
    if (!std::isfinite(config.reconnect.jitter_min) ||
        !std::isfinite(config.reconnect.jitter_max) ||
        config.reconnect.jitter_min <= 0.0 ||
        config.reconnect.jitter_max < config.reconnect.jitter_min) {
        return invalid("reconnect jitter range is invalid");
    }
    if (config.artifact_limits.maximum_map_bytes == 0 ||
        config.artifact_limits.maximum_road_network_bytes == 0 ||
        config.artifact_limits.stream_chunk_bytes == 0) {
        return invalid("artifact limits must be positive");
    }
    if (config.tls.insecure_demo &&
        (config.tls.verify_peer || config.tls.verify_hostname)) {
        return invalid("insecure_demo requires peer and hostname verification to be disabled");
    }
    if (starts_with(config.base_url, "https://") && !config.tls.insecure_demo &&
        (!config.tls.verify_peer || !config.tls.verify_hostname)) {
        return invalid("https requires peer and hostname verification unless insecure_demo is explicit");
    }
    return {};
}

}  // namespace roc::vehicle
