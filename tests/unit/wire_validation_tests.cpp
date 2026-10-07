#include "contract_validation.hpp"
#include <jsoncons/json.hpp>
#include <iostream>
#include <string>
#include <vector>

namespace {
using Json = jsoncons::json;
using namespace roc::vehicle::detail;
int failures = 0;
#define EXPECT(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while (false)
bool classify(const Json& j) {
    const auto body = j.at("json").as<std::string>();
    const auto family = j.at("family").as<std::string>();
    if (family == "device") return valid_device_contract(body, j.at("definition").as<std::string>());
    if (family == "task") return valid_task_contract(body, j.at("definition").as<std::string>());
    if (family == "road") return valid_road_contract(body, j.at("version").as<int>());
    return false;
}
}
int main(int argc, char** argv) {
    // Test-only classifier enables a private differential oracle without making
    // the public test/build depend on external documents or reference snapshots.
    if (argc == 2 && std::string{argv[1]} == "--classify") {
        std::string line;
        while (std::getline(std::cin, line)) {
            try { std::cout << (classify(Json::parse(line)) ? '1' : '0') << '\n'; }
            catch (const std::exception&) { std::cout << "0\n"; }
        }
        return 0;
    }
    EXPECT(wire_rules_self_test());
    for (const auto* s : {"1", "9223372036854775807"}) EXPECT(valid_device_contract(Json{s}.to_string(), "positiveInt64String"));
    for (const auto* s : {"0", "01", "+1", "-1", "9223372036854775808", "1x"}) EXPECT(!valid_device_contract(Json{s}.to_string(), "positiveInt64String"));
    EXPECT(valid_device_contract("{}", "heartbeatPayload"));
    EXPECT(!valid_device_contract(R"({"extra":1})", "heartbeatPayload"));
    EXPECT(!valid_task_contract("{}", "unknownDefinition"));
    auto status = Json::parse(R"({"event_id":"123e4567-e89b-12d3-a456-426614174001","state":"delivered","progress":100})");
    EXPECT(valid_task_contract(status.to_string(), "statusRequest"));
    status["progress"] = 99; EXPECT(!valid_task_contract(status.to_string(), "statusRequest"));
    status["state"] = "failed"; EXPECT(!valid_task_contract(status.to_string(), "statusRequest"));
    status["error_code"] = "synthetic"; EXPECT(valid_task_contract(status.to_string(), "statusRequest"));
    status["error_code"] = ""; EXPECT(!valid_task_contract(status.to_string(), "statusRequest"));
    status["state"] = "downloading"; EXPECT(valid_task_contract(status.to_string(), "statusRequest"));
    status["unknown"] = true; EXPECT(!valid_task_contract(status.to_string(), "statusRequest"));
    auto hello = Json::parse(R"({"protocol_version":1,"message_id":"123e4567-e89b-12d3-a456-426614174001","type":"hello",
        "sequence":"1","timestamp":"2026-10-08T00:00:00Z","payload":{"vehicle_id":"123e4567-e89b-12d3-a456-426614174002",
        "heartbeat_interval_seconds":5,"idle_timeout_seconds":30,"max_message_bytes":65536,"max_heartbeat_bytes":4096,
        "max_telemetry_bytes":16384,"last_client_sequence":"0"}})");
    EXPECT(valid_device_contract(hello.to_string(), "hello"));
    hello["payload"]["future_optional"] = true; EXPECT(valid_device_contract(hello.to_string(), "hello"));
    hello["payload"]["heartbeat_interval_seconds"] = 3601; EXPECT(!valid_device_contract(hello.to_string(), "hello"));
    hello["payload"]["heartbeat_interval_seconds"] = 3600; hello["extra"] = 1; EXPECT(!valid_device_contract(hello.to_string(), "hello"));
    auto road = Json::parse(R"({"schema_version":2,"coordinate_mode":"legacy-normalized","sampling":{"algorithm":"uniform-parameter-v1","spacing":0.000001,"precision":6},"nodes":[],"edges":[],"trajectories":[]})");
    EXPECT(valid_road_contract(road.to_string(), 2));
    road["sampling"]["spacing"] = 1; EXPECT(valid_road_contract(road.to_string(), 2));
    road["sampling"]["spacing"] = 1.1; EXPECT(!valid_road_contract(road.to_string(), 2));
    road["coordinate_mode"] = "metric"; EXPECT(valid_road_contract(road.to_string(), 2));
    road["sampling"]["precision"] = 5; EXPECT(!valid_road_contract(road.to_string(), 2));
    EXPECT(!valid_road_contract("{}", 3));
    std::cout << "wire_validation failures=" << failures << '\n';
    return failures == 0 ? 0 : 1;
}
