#include "roc/vehicle/road_network.hpp"

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

std::string valid_network() {
    return R"JSON({
      "schema_version":1,
      "coordinate_mode":"metric",
      "nodes":[
        {"id":"node-b","x":2.0,"y":3.0,"kind":"waypoint","label":"B"},
        {"id":"node-a","x":1.0,"y":1.5,"kind":"waypoint","label":"A"}
      ],
      "edges":[
        {"id":"edge-a","from":"node-a","to":"node-b","direction":"both",
         "max_speed_mps":2.5}
      ]
    })JSON";
}

void test_valid_and_mode() {
    const auto result = roc::vehicle::validate_road_network_v1(
        valid_network(), "metric");
    EXPECT(result);
    if (result) {
        EXPECT(result.value().node_count == 2);
        EXPECT(result.value().edge_count == 1);
        EXPECT(result.value().coordinate_mode == "metric");
    }
    const auto mismatch = roc::vehicle::validate_road_network_v1(
        valid_network(), "legacy-normalized");
    EXPECT(!mismatch);
    if (!mismatch) {
        EXPECT(mismatch.error().server_code == "coordinate_mode_mismatch");
    }
}

void test_current_system_semantics() {
    auto dangling = valid_network();
    const auto to = dangling.find("\"to\":\"node-b\"");
    dangling.replace(to, 13, "\"to\":\"missing\"");
    const auto dangling_result = roc::vehicle::validate_road_network_v1(dangling);
    EXPECT(!dangling_result);
    if (!dangling_result) {
        EXPECT(dangling_result.error().server_code == "dangling_edge");
    }

    auto self_loop = valid_network();
    const auto self_to = self_loop.find("\"to\":\"node-b\"");
    self_loop.replace(self_to, 13, "\"to\":\"node-a\"");
    const auto loop_result = roc::vehicle::validate_road_network_v1(self_loop);
    EXPECT(!loop_result);
    if (!loop_result) {
        EXPECT(loop_result.error().server_code == "self_loop");
    }

    auto speed = valid_network();
    const auto speed_value = speed.find("2.5");
    speed.replace(speed_value, 3, "101");
    const auto speed_result = roc::vehicle::validate_road_network_v1(speed);
    EXPECT(!speed_result);
    if (!speed_result) {
        EXPECT(speed_result.error().server_code == "invalid_edge");
    }

    auto unknown = valid_network();
    const auto root_end = unknown.rfind('}');
    unknown.insert(root_end, ",\"unexpected\":true");
    EXPECT(!roc::vehicle::validate_road_network_v1(unknown));
}

void test_duplicate_connection() {
    auto duplicate = valid_network();
    const auto edges_end = duplicate.rfind(']');
    duplicate.insert(
        edges_end,
        R"JSON(,{"id":"edge-b","from":"node-b","to":"node-a","direction":"both"})JSON");
    const auto result = roc::vehicle::validate_road_network_v1(duplicate);
    EXPECT(!result);
    if (!result) {
        EXPECT(result.error().server_code == "duplicate_edge");
    }
}

}  // namespace

int main() {
    test_valid_and_mode();
    test_current_system_semantics();
    test_duplicate_connection();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
