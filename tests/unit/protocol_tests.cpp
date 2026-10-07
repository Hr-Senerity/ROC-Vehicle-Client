#include "roc/vehicle/protocol.hpp"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <variant>

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

constexpr auto kMessageId = "123e4567-e89b-12d3-a456-426614174000";
constexpr auto kVehicleId = "123e4567-e89b-12d3-a456-426614174001";
constexpr auto kTimestamp = "2026-09-26T12:00:00.000Z";

std::string read_fixture(const char* relative_path) {
    const auto path = std::filesystem::path{ROC_VEHICLE_TEST_FIXTURE_DIR} / relative_path;
    std::ifstream input{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

void test_sequence() {
    EXPECT(roc::vehicle::parse_positive_sequence("1").value() == 1);
    EXPECT(roc::vehicle::parse_positive_sequence("9223372036854775807").value() ==
           std::numeric_limits<std::int64_t>::max());
    EXPECT(!roc::vehicle::parse_positive_sequence("0"));
    EXPECT(!roc::vehicle::parse_positive_sequence("01"));
    EXPECT(!roc::vehicle::parse_positive_sequence("9223372036854775808"));
    EXPECT(roc::vehicle::parse_nonnegative_cursor("0").value() == 0);
}

void test_uuid_and_timestamp() {
    const auto generated = roc::vehicle::generate_uuid_v4();
    EXPECT(generated);
    if (generated) {
        EXPECT(roc::vehicle::is_valid_uuid(generated.value()));
        EXPECT(generated.value()[14] == '4');
        EXPECT(generated.value()[19] == '8' || generated.value()[19] == '9' ||
               generated.value()[19] == 'a' || generated.value()[19] == 'b');
    }

    EXPECT(roc::vehicle::is_valid_rfc3339_timestamp("2024-02-29T23:59:60Z"));
    EXPECT(roc::vehicle::is_valid_rfc3339_timestamp("2026-09-26T12:00:00.123+08:00"));
    EXPECT(!roc::vehicle::is_valid_rfc3339_timestamp("2025-02-29T12:00:00Z"));
    EXPECT(!roc::vehicle::is_valid_rfc3339_timestamp("2026-13-01T12:00:00Z"));
    EXPECT(!roc::vehicle::is_valid_rfc3339_timestamp("2026-09-26T24:00:00Z"));
    EXPECT(!roc::vehicle::is_valid_rfc3339_timestamp("2026-09-26T12:00:00+24:00"));
    EXPECT(!roc::vehicle::is_valid_rfc3339_timestamp("2026-09-26t12:00:00Z"));
    EXPECT(!roc::vehicle::is_valid_rfc3339_timestamp(
        "2026-09-26T12:00:00.1234567890Z"));
}

void test_hello_and_optional_fields() {
    const auto hello = read_fixture("device-protocol/hello-with-optional.json");
    EXPECT(!hello.empty());
    const auto parsed = roc::vehicle::parse_downstream_message(hello);
    EXPECT(parsed);
    EXPECT(std::holds_alternative<roc::vehicle::HelloMessage>(parsed.value()));
    if (parsed && std::holds_alternative<roc::vehicle::HelloMessage>(parsed.value())) {
        const auto& message = std::get<roc::vehicle::HelloMessage>(parsed.value());
        EXPECT(message.vehicle_id == kVehicleId);
        EXPECT(message.last_client_sequence == 0);
    }

    const std::string bad_envelope = R"JSON({
      "protocol_version":1,
      "message_id":"123e4567-e89b-12d3-a456-426614174000",
      "type":"hello",
      "sequence":"1",
      "timestamp":"2026-09-26T12:00:00.000Z",
      "payload":{
        "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
        "heartbeat_interval_seconds":30,
        "idle_timeout_seconds":45,
        "max_message_bytes":65536,
        "max_heartbeat_bytes":4096,
        "max_telemetry_bytes":16384,
        "last_client_sequence":"0"
      },
      "unexpected":true
    })JSON";
    EXPECT(!roc::vehicle::parse_downstream_message(bad_envelope));
}

void test_task_strict_payload() {
    const auto valid = read_fixture("deployment-task/task-available-road-network.json");
    EXPECT(!valid.empty());
    const auto parsed = roc::vehicle::parse_downstream_message(valid);
    EXPECT(parsed);
    EXPECT(std::holds_alternative<roc::vehicle::TaskAvailableMessage>(parsed.value()));

    auto invalid = valid;
    const auto insertion = invalid.find("\"max_attempts\"");
    EXPECT(insertion != std::string::npos);
    if (insertion != std::string::npos) {
        invalid.insert(insertion, "\"unknown\": true,\n    ");
        EXPECT(roc::vehicle::parse_downstream_message(invalid));
    }
}

void test_upstream_encoding() {
    const auto heartbeat = roc::vehicle::make_heartbeat_message(
        kMessageId, 7, kTimestamp, "roc-vehicle-cpp-0.1.0");
    EXPECT(heartbeat);
    EXPECT(heartbeat.value().find("\"sequence\":\"7\"") != std::string::npos);
    EXPECT(heartbeat.value().find("\"type\":\"heartbeat\"") != std::string::npos);

    roc::vehicle::TelemetrySnapshot telemetry;
    telemetry.online = true;
    telemetry.cpu_usage = 20.5;
    telemetry.memory_usage = 31.0;
    telemetry.battery_level = 88;
    telemetry.localization_confidence = 97.5;
    telemetry.position = {1.2, 3.4, 0.5};
    telemetry.velocity = {0.3, 0.05};
    telemetry.library_version = "roc-vehicle-cpp-0.1.0";
    EXPECT(roc::vehicle::make_telemetry_message(kMessageId, 8, kTimestamp, telemetry));

    telemetry.position.x = std::numeric_limits<double>::quiet_NaN();
    EXPECT(!roc::vehicle::make_telemetry_message(kMessageId, 9, kTimestamp, telemetry));
}

}  // namespace

int main() {
    test_sequence();
    test_uuid_and_timestamp();
    test_hello_and_optional_fields();
    test_task_strict_payload();
    test_upstream_encoding();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
