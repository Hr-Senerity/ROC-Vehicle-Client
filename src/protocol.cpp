#include "roc/vehicle/protocol.hpp"
#include "contract_validation.hpp"

#include <jsoncons/json.hpp>

#include <array>
#include <charconv>
#include <cmath>
#include <limits>
#include <random>
#include <regex>
#include <set>
#include <string>
#include <system_error>

namespace roc::vehicle {
namespace {

using Json = jsoncons::json;

Result<DownstreamMessage> protocol_error(std::string message) {
    return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                 std::move(message)};
}

Result<std::string> encode_error(std::string message) {
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

Result<std::string> required_string(const Json& object, std::string_view key) {
    if (!object.contains(key) || !object.at(key).is_string()) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "missing or invalid string field: " + std::string{key}};
    }
    return object.at(key).as<std::string>();
}

Result<std::int64_t> required_integer(const Json& object, std::string_view key,
                                      std::int64_t minimum,
                                      std::int64_t maximum) {
    if (!object.contains(key)) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "missing integer field: " + std::string{key}};
    }
    const auto& value = object.at(key);
    std::int64_t parsed = 0;
    if (value.is_int64()) {
        parsed = value.as<std::int64_t>();
    } else if (value.is_uint64()) {
        const auto unsigned_value = value.as<std::uint64_t>();
        if (unsigned_value > static_cast<std::uint64_t>(maximum)) {
            return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                         "integer field out of range: " + std::string{key}};
        }
        parsed = static_cast<std::int64_t>(unsigned_value);
    } else {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "invalid integer field: " + std::string{key}};
    }
    if (parsed < minimum || parsed > maximum) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "integer field out of range: " + std::string{key}};
    }
    return parsed;
}

Result<EnvelopeMetadata> parse_envelope(const Json& root) {
    static const std::set<std::string> fields{
        "protocol_version", "message_id", "type", "sequence", "timestamp", "payload"};
    if (!has_exact_members(root, fields)) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "downstream envelope fields are invalid"};
    }
    const auto version = required_integer(root, "protocol_version", 1, 1);
    if (!version) {
        return version.error();
    }
    const auto message_id = required_string(root, "message_id");
    if (!message_id || !is_valid_uuid(message_id.value())) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "message_id is not a UUID"};
    }
    const auto sequence_text = required_string(root, "sequence");
    if (!sequence_text) {
        return sequence_text.error();
    }
    const auto sequence = parse_positive_sequence(sequence_text.value());
    if (!sequence) {
        return sequence.error();
    }
    const auto timestamp = required_string(root, "timestamp");
    if (!timestamp || !is_valid_rfc3339_timestamp(timestamp.value())) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "timestamp is not RFC 3339"};
    }
    if (!root.at("payload").is_object()) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "payload must be an object"};
    }
    return EnvelopeMetadata{message_id.value(), sequence.value(), timestamp.value()};
}

Result<DownstreamMessage> parse_hello(const Json& root,
                                      const EnvelopeMetadata& envelope) {
    const auto& payload = root.at("payload");
    const auto vehicle_id = required_string(payload, "vehicle_id");
    if (!vehicle_id || !is_valid_uuid(vehicle_id.value())) {
        return protocol_error("hello vehicle_id is not a UUID");
    }
    const auto heartbeat = required_integer(payload, "heartbeat_interval_seconds", 1,
                                            std::numeric_limits<std::int32_t>::max());
    const auto idle = required_integer(payload, "idle_timeout_seconds", 1,
                                       std::numeric_limits<std::int32_t>::max());
    const auto message_limit = required_integer(payload, "max_message_bytes", 1,
                                                std::numeric_limits<std::int32_t>::max());
    const auto heartbeat_limit = required_integer(payload, "max_heartbeat_bytes", 1,
                                                  std::numeric_limits<std::int32_t>::max());
    const auto telemetry_limit = required_integer(payload, "max_telemetry_bytes", 1,
                                                  std::numeric_limits<std::int32_t>::max());
    const auto last_text = required_string(payload, "last_client_sequence");
    if (!heartbeat || !idle || !message_limit || !heartbeat_limit || !telemetry_limit ||
        !last_text) {
        return protocol_error("hello payload is incomplete or invalid");
    }
    const auto last = parse_nonnegative_cursor(last_text.value());
    if (!last) {
        return protocol_error("hello last_client_sequence is invalid");
    }
    if (heartbeat_limit.value() > message_limit.value() ||
        telemetry_limit.value() > message_limit.value()) {
        return protocol_error("hello message limits are inconsistent");
    }
    return DownstreamMessage{HelloMessage{
        envelope, vehicle_id.value(), heartbeat.value(), idle.value(), message_limit.value(),
        heartbeat_limit.value(), telemetry_limit.value(), last.value()}};
}

Result<DownstreamMessage> parse_ack(const Json& root,
                                    const EnvelopeMetadata& envelope) {
    const auto& payload = root.at("payload");
    const auto message_id = required_string(payload, "ack_message_id");
    const auto sequence_text = required_string(payload, "ack_sequence");
    const auto accepted = required_string(payload, "accepted_type");
    if (!message_id || !is_valid_uuid(message_id.value()) || !sequence_text || !accepted ||
        !payload.contains("duplicate") || !payload.at("duplicate").is_bool()) {
        return protocol_error("ack payload is incomplete or invalid");
    }
    const auto sequence = parse_positive_sequence(sequence_text.value());
    if (!sequence) {
        return protocol_error("ack sequence is invalid");
    }
    AcceptedMessageType type{};
    if (accepted.value() == "heartbeat") {
        type = AcceptedMessageType::heartbeat;
    } else if (accepted.value() == "telemetry") {
        type = AcceptedMessageType::telemetry;
    } else {
        return protocol_error("ack accepted_type is invalid");
    }
    return DownstreamMessage{AckMessage{envelope, message_id.value(), sequence.value(), type,
                                        payload.at("duplicate").as<bool>()}};
}

Result<DownstreamMessage> parse_error_message(const Json& root,
                                              const EnvelopeMetadata& envelope) {
    const auto& payload = root.at("payload");
    const auto code = required_string(payload, "code");
    const auto message = required_string(payload, "message");
    if (!code || code.value().empty() || code.value().size() > 64 || !message ||
        message.value().size() > 4096) {
        return protocol_error("error payload is invalid");
    }
    std::string request_message_id;
    if (payload.contains("request_message_id")) {
        const auto request = required_string(payload, "request_message_id");
        if (!request || !is_valid_uuid(request.value())) {
            return protocol_error("error request_message_id is invalid");
        }
        request_message_id = request.value();
    }
    return DownstreamMessage{ErrorMessage{envelope, code.value(), message.value(),
                                          std::move(request_message_id)}};
}

Result<DownstreamMessage> parse_task(const Json& root,
                                     const EnvelopeMetadata& envelope) {
    const auto& payload = root.at("payload");
    const auto task_id = required_string(payload, "task_id");
    const auto batch_id = required_string(payload, "batch_id");
    const auto project_id = required_string(payload, "project_id");
    const auto revision_id = required_string(payload, "resource_revision_id");
    const auto resource = required_string(payload, "resource_type");
    const auto attempt = required_integer(payload, "attempt", 0,
                                          std::numeric_limits<std::int32_t>::max());
    const auto max_attempts = required_integer(payload, "max_attempts", 1, 10);
    if (!task_id || !batch_id || !project_id || !revision_id || !resource || !attempt ||
        !max_attempts || !is_valid_uuid(task_id.value()) || !is_valid_uuid(batch_id.value()) ||
        !is_valid_uuid(project_id.value()) || !is_valid_uuid(revision_id.value())) {
        return protocol_error("task.available payload is invalid");
    }
    ResourceType resource_type{};
    if (resource.value() == "map") {
        resource_type = ResourceType::map;
    } else if (resource.value() == "road_network") {
        resource_type = ResourceType::road_network;
    } else {
        return protocol_error("task.available resource_type is invalid");
    }
    return DownstreamMessage{TaskAvailableMessage{
        envelope, task_id.value(), batch_id.value(), project_id.value(), resource_type,
        revision_id.value(), attempt.value(), max_attempts.value()}};
}

Result<void> validate_upstream_envelope(std::string_view message_id, Sequence sequence,
                                        std::string_view timestamp) {
    if (!is_valid_uuid(message_id)) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "message_id is not a UUID"};
    }
    if (sequence <= 0) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "sequence must be a positive INT64"};
    }
    if (!is_valid_rfc3339_timestamp(timestamp)) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "timestamp is not RFC 3339"};
    }
    return {};
}

bool valid_percentage(double value) {
    return std::isfinite(value) && value >= 0.0 && value <= 100.0;
}

bool valid_library_version(std::string_view value) {
    if (value.empty()) {
        return true;
    }
    static const std::regex pattern{"^[A-Za-z0-9][A-Za-z0-9._+-]{0,63}$"};
    return std::regex_match(value.begin(), value.end(), pattern);
}

Json base_upstream_message(std::string_view type, std::string_view message_id,
                           Sequence sequence, std::string_view timestamp) {
    Json root(jsoncons::json_object_arg);
    root["protocol_version"] = 1;
    root["message_id"] = std::string{message_id};
    root["type"] = std::string{type};
    root["sequence"] = std::to_string(sequence);
    root["timestamp"] = std::string{timestamp};
    return root;
}

}  // namespace

Result<Sequence> parse_positive_sequence(std::string_view text) {
    if (text.empty() || text.front() == '0') {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "sequence must be a canonical positive INT64"};
    }
    Sequence value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value <= 0) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "sequence must be a canonical positive INT64"};
    }
    return value;
}

Result<std::int64_t> parse_nonnegative_cursor(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "cursor must be a canonical nonnegative INT64"};
    }
    std::int64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || value < 0) {
        return Error{ErrorDomain::protocol, ErrorCode::invalid_argument, false,
                     "cursor must be a canonical nonnegative INT64"};
    }
    return value;
}

bool is_valid_uuid(std::string_view text) noexcept {
    if (text.size() != 36) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (text[i] != '-') {
                return false;
            }
        } else if (std::isxdigit(static_cast<unsigned char>(text[i])) == 0) {
            return false;
        }
    }
    return true;
}

Result<std::string> generate_uuid_v4() {
    try {
        std::random_device entropy;
        std::array<unsigned char, 16> bytes{};
        for (auto& byte : bytes) {
            byte = static_cast<unsigned char>(entropy() & 0xffU);
        }
        bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0fU) | 0x40U);
        bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3fU) | 0x80U);

        static constexpr char hexadecimal[] = "0123456789abcdef";
        std::string result;
        result.reserve(36);
        for (std::size_t index = 0; index < bytes.size(); ++index) {
            if (index == 4 || index == 6 || index == 8 || index == 10) {
                result.push_back('-');
            }
            result.push_back(hexadecimal[(bytes[index] >> 4U) & 0x0fU]);
            result.push_back(hexadecimal[bytes[index] & 0x0fU]);
        }
        return result;
    } catch (const std::exception&) {
        return Error{ErrorDomain::protocol, ErrorCode::io_failure, false,
                     "unable to obtain UUID entropy"};
    }
}

bool is_valid_rfc3339_timestamp(std::string_view text) {
    const auto digits = [text](std::size_t offset, std::size_t count, int& value) {
        if (offset + count > text.size()) {
            return false;
        }
        value = 0;
        for (std::size_t index = offset; index < offset + count; ++index) {
            const char character = text[index];
            if (character < '0' || character > '9') {
                return false;
            }
            value = value * 10 + (character - '0');
        }
        return true;
    };
    if (text.size() < 20 || text[4] != '-' || text[7] != '-' ||
        text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return false;
    }

    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!digits(0, 4, year) || !digits(5, 2, month) || !digits(8, 2, day) ||
        !digits(11, 2, hour) || !digits(14, 2, minute) || !digits(17, 2, second) ||
        month < 1 || month > 12 || hour > 23 || minute > 59 || second > 60) {
        return false;
    }
    static constexpr std::array<int, 12> month_days{
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int maximum_day = month_days[static_cast<std::size_t>(month - 1)];
    const bool leap_year = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (month == 2 && leap_year) {
        maximum_day = 29;
    }
    if (day < 1 || day > maximum_day) {
        return false;
    }

    std::size_t timezone = 19;
    if (timezone < text.size() && text[timezone] == '.') {
        ++timezone;
        const auto fraction_start = timezone;
        while (timezone < text.size() && text[timezone] >= '0' && text[timezone] <= '9') {
            ++timezone;
        }
        if (timezone == fraction_start || timezone - fraction_start > 9) {
            return false;
        }
    }
    if (timezone + 1 == text.size() && text[timezone] == 'Z') {
        return true;
    }
    if (timezone + 6 != text.size() ||
        (text[timezone] != '+' && text[timezone] != '-') || text[timezone + 3] != ':') {
        return false;
    }
    int offset_hour = 0;
    int offset_minute = 0;
    return digits(timezone + 1, 2, offset_hour) &&
           digits(timezone + 4, 2, offset_minute) &&
           offset_hour <= 23 && offset_minute <= 59;
}

Result<DownstreamMessage> parse_downstream_message(std::string_view json_text) {
    if (json_text.empty() || json_text.size() > 65536) {
        return protocol_error("downstream message size is invalid");
    }
    try {
        const auto root = Json::parse(json_text);
        const auto envelope = parse_envelope(root);
        if (!envelope) {
            return envelope.error();
        }
        const auto type = required_string(root, "type");
        if (!type) {
            return protocol_error("message type is invalid");
        }
        if (type.value() == "hello") {
            if (!detail::valid_device_contract(json_text, "hello"))
                return protocol_error("hello violates the fixed contract");
            return parse_hello(root, envelope.value());
        }
        if (type.value() == "ack") {
            if (!detail::valid_device_contract(json_text, "ack"))
                return protocol_error("ack violates the fixed contract");
            return parse_ack(root, envelope.value());
        }
        if (type.value() == "error") {
            if (!detail::valid_device_contract(json_text, "error"))
                return protocol_error("error violates the fixed contract");
            return parse_error_message(root, envelope.value());
        }
        if (type.value() == "task.available") {
            if (!detail::valid_task_contract(json_text, "taskAvailable"))
                return protocol_error("task.available violates the fixed contract");
            return parse_task(root, envelope.value());
        }
        return protocol_error("unsupported downstream message type");
    } catch (const std::exception&) {
        return protocol_error("downstream JSON is invalid");
    }
}

Result<std::string> make_heartbeat_message(std::string_view message_id, Sequence sequence,
                                           std::string_view timestamp,
                                           std::string_view library_version) {
    const auto valid = validate_upstream_envelope(message_id, sequence, timestamp);
    if (!valid) {
        return valid.error();
    }
    if (!valid_library_version(library_version)) {
        return encode_error("library_version is invalid");
    }
    Json root = base_upstream_message("heartbeat", message_id, sequence, timestamp);
    Json payload(jsoncons::json_object_arg);
    if (!library_version.empty()) {
        payload["library_version"] = std::string{library_version};
    }
    root["payload"] = std::move(payload);
    const auto encoded = root.to_string();
    if (encoded.size() > 4096) {
        return encode_error("heartbeat exceeds 4096 bytes");
    }
    return encoded;
}

Result<std::string> make_telemetry_message(std::string_view message_id, Sequence sequence,
                                           std::string_view timestamp,
                                           const TelemetrySnapshot& telemetry) {
    const auto valid = validate_upstream_envelope(message_id, sequence, timestamp);
    if (!valid) {
        return valid.error();
    }
    if (!valid_library_version(telemetry.library_version) ||
        !valid_percentage(telemetry.cpu_usage) ||
        !valid_percentage(telemetry.memory_usage) ||
        telemetry.battery_level < 0 || telemetry.battery_level > 100 ||
        !valid_percentage(telemetry.localization_confidence) ||
        !std::isfinite(telemetry.position.x) || !std::isfinite(telemetry.position.y) ||
        !std::isfinite(telemetry.position.theta) || !std::isfinite(telemetry.velocity.linear) ||
        !std::isfinite(telemetry.velocity.angular)) {
        return encode_error("telemetry payload is invalid");
    }
    Json root = base_upstream_message("telemetry", message_id, sequence, timestamp);
    Json payload(jsoncons::json_object_arg);
    if (!telemetry.library_version.empty()) {
        payload["library_version"] = telemetry.library_version;
    }
    payload["online"] = telemetry.online;
    payload["cpu_usage"] = telemetry.cpu_usage;
    payload["memory_usage"] = telemetry.memory_usage;
    payload["battery_level"] = telemetry.battery_level;
    payload["localization_confidence"] = telemetry.localization_confidence;
    Json position(jsoncons::json_object_arg);
    position["x"] = telemetry.position.x;
    position["y"] = telemetry.position.y;
    position["theta"] = telemetry.position.theta;
    payload["position"] = std::move(position);
    Json velocity(jsoncons::json_object_arg);
    velocity["linear"] = telemetry.velocity.linear;
    velocity["angular"] = telemetry.velocity.angular;
    payload["velocity"] = std::move(velocity);
    root["payload"] = std::move(payload);
    const auto encoded = root.to_string();
    if (encoded.size() > 16384) {
        return encode_error("telemetry exceeds 16384 bytes");
    }
    return encoded;
}

}  // namespace roc::vehicle
