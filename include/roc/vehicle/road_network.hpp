#pragma once

#include "roc/vehicle/result.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <optional>
#include <vector>

namespace roc::vehicle {

struct RoadNetworkSummary {
    std::string coordinate_mode;
    std::size_t node_count{0};
    std::size_t edge_count{0};
};

// Mirrors ROC-SYSTEM v0.2.1 validation semantics. When
// expected_coordinate_mode is empty, the document mode is still validated but
// no external map-mode comparison can be made.
[[nodiscard]] Result<RoadNetworkSummary> validate_road_network_v1(
    std::string_view json_text,
    std::string_view expected_coordinate_mode = {});

struct RoadPoint { double x{0}, y{0}; };
struct RoadNode { std::string id; RoadPoint position; std::string kind, label; };
struct RoadGeometry {
    std::string type;
    std::optional<RoadPoint> control1, control2;
};
struct RoadEdge {
    std::string id, from, to, direction;
    std::optional<double> max_speed_mps;
    RoadGeometry geometry;
};
struct RoadSample {
    std::size_t index{0};
    double x{0}, y{0}, s{0}, heading{0};
};
struct RoadTrajectory {
    std::string id, edge_id, direction;
    std::optional<double> max_speed_mps;
    double length{0};
    std::vector<RoadSample> points;
};
struct RoadSampling { std::string algorithm; double spacing{0}; int precision{0}; };
struct RoadNetworkV2 {
    std::string coordinate_mode;
    RoadSampling sampling;
    std::vector<RoadNode> nodes;
    std::vector<RoadEdge> edges;
    std::vector<RoadTrajectory> trajectories;
};
struct RoadValidationOptions {
    std::string expected_coordinate_mode;
    std::size_t maximum_document_bytes{64U*1024U*1024U};
};
// Input order and samples are authoritative; never reconstruct or resample.
// Point/count/depth preflight precedes DOM and typed allocations.
[[nodiscard]] Result<RoadNetworkV2> parse_road_network_v2(
    std::string_view json_text, RoadValidationOptions options = {});

}  // namespace roc::vehicle
