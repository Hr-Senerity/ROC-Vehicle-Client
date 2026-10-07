#include "roc/vehicle/task_protocol.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

constexpr auto kTaskId = "123e4567-e89b-12d3-a456-426614174010";

roc::vehicle::TaskAvailableMessage notification(roc::vehicle::ResourceType type) {
    return {{"123e4567-e89b-12d3-a456-426614174000", 1,
             "2026-09-26T12:00:00Z"},
            kTaskId,
            "123e4567-e89b-12d3-a456-426614174011",
            "123e4567-e89b-12d3-a456-426614174012",
            type,
            "123e4567-e89b-12d3-a456-426614174013",
            0,
            3};
}

void test_accept_adapter() {
    const std::string response = R"JSON({
      "ok":true,
      "replayed":false,
      "lease_token":"opaque-test-lease",
      "lease_expires_at":1790413260,
      "task":{
        "id":"123e4567-e89b-12d3-a456-426614174010",
        "state":"accepted",
        "attempt":1,
        "max_attempts":3,
        "progress":20,"batch_id":"123e4567-e89b-12d3-a456-426614174011",
          "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
          "error_code":null,"error_message":null,"offered_at":null,
          "accepted_at":null,"delivered_at":null,
          "updated_at":"2026-10-04T00:00:00Z",
        "future_optional_field":true
      },
      "future_top_level_field":true
    })JSON";
    const auto parsed = roc::vehicle::parse_accept_response(response, kTaskId);
    EXPECT(parsed);
    if (parsed) {
        EXPECT(parsed.value().attempt == 1);
        EXPECT(parsed.value().progress_floor == 20);
        EXPECT(!parsed.value().replayed);
        EXPECT(!parsed.value().is_expired_at(1790413260));
        EXPECT(parsed.value().is_expired_at(1790413261));
    }
    auto wrong = response;
    const auto offset = wrong.find(kTaskId);
    EXPECT(offset != std::string::npos);
    if (offset != std::string::npos) {
        wrong.replace(offset, 36, "123e4567-e89b-12d3-a456-426614174099");
        EXPECT(!roc::vehicle::parse_accept_response(wrong, kTaskId));
    }
}

void test_manifest_adapter() {
    const auto road = notification(roc::vehicle::ResourceType::road_network);
    const std::string response = R"JSON({
      "ok":true,
      "manifest":{
        "task_id":"123e4567-e89b-12d3-a456-426614174010",
        "batch_id":"123e4567-e89b-12d3-a456-426614174011",
        "project_id":"123e4567-e89b-12d3-a456-426614174012",
        "resource_type":"road_network",
        "resource_revision_id":"123e4567-e89b-12d3-a456-426614174013",
        "map_id":"123e4567-e89b-12d3-a456-426614174014",
        "content_type":"application/vnd.roc.road-network+json",
        "byte_size":1234,
        "sha256":"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
        "version":2,
        "schema_version":1,
        "artifact_url":"/api/device/tasks/123e4567-e89b-12d3-a456-426614174010/artifact"
      }
    })JSON";
    roc::vehicle::ArtifactLimits limits;
    const auto parsed = roc::vehicle::parse_manifest_response(response, road, limits);
    EXPECT(parsed);
    if (parsed) {
        EXPECT(parsed.value().byte_size == 1234);
        EXPECT(parsed.value().schema_version == 1);
    }

    auto bad_schema = response;
    const auto schema = bad_schema.find("\"schema_version\":1");
    EXPECT(schema != std::string::npos);
    if (schema != std::string::npos) {
        bad_schema.replace(schema, 18, "\"schema_version\":2");
        EXPECT(!roc::vehicle::parse_manifest_response(bad_schema, road, limits));
    }
    limits.maximum_road_network_bytes = 100;
    const auto oversized = roc::vehicle::parse_manifest_response(response, road, limits);
    EXPECT(!oversized);
    if (!oversized) {
        EXPECT(oversized.error().domain == roc::vehicle::ErrorDomain::artifact);
    }
}

void test_status_adapter() {
    roc::vehicle::StatusEventRecord event{
        "123e4567-e89b-12d3-a456-426614174020", {kTaskId, 1},
        roc::vehicle::TaskExecutionState::failed, 42, "hash_mismatch",
        "artifact digest differs"};
    const auto encoded = roc::vehicle::make_status_request(event);
    EXPECT(encoded);
    if (encoded) {
        EXPECT(encoded.value().find("\"error_code\":\"hash_mismatch\"") !=
               std::string::npos);
    }
    const auto response = roc::vehicle::parse_status_response(
        R"JSON({"ok":true,"replayed":true,
    "project_id":"123e4567-e89b-12d3-a456-426614174012",
    "task":{"id":"123e4567-e89b-12d3-a456-426614174010",
    "state":"delivered","attempt":2,"max_attempts":3,"progress":100,"batch_id":"123e4567-e89b-12d3-a456-426614174011",
          "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
          "error_code":null,"error_message":null,"offered_at":null,
          "accepted_at":null,"delivered_at":null,
          "updated_at":"2026-10-04T00:00:00Z"}})JSON");
    EXPECT(response);
    if (response) {
        EXPECT(response.value().replayed);
    }
    const auto error = roc::vehicle::parse_http_error_response(
        R"JSON({"ok":false,"code":"lease_expired","message":"expired"})JSON");
    EXPECT(error);
    if (error) {
        EXPECT(error.value().code == "lease_expired");
    }
}

void test_fixed_targets() {
    EXPECT(roc::vehicle::task_accept_target(kTaskId) ==
           std::string{"/api/device/tasks/"} + kTaskId + "/accept");
    EXPECT(roc::vehicle::task_artifact_target("../escape").empty());
}

}  // namespace

int main() {
    test_accept_adapter();
    test_manifest_adapter();
    test_status_adapter();
    test_fixed_targets();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
