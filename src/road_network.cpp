#include "roc/vehicle/road_network.hpp"
#include "contract_validation.hpp"

#include <jsoncons/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <regex>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace roc::vehicle {
namespace {

using Json = jsoncons::json;

constexpr std::size_t kMaximumNodes = 10000;
constexpr std::size_t kMaximumEdges = 50000;
constexpr std::size_t kMaximumDocumentBytes = 10U * 1024U * 1024U;
constexpr double kMaximumSpeedMps = 100.0;

Result<RoadNetworkSummary> invalid(std::string code, std::string message) {
    return Error{ErrorDomain::artifact, ErrorCode::integrity_failure, false,
                 std::move(message), std::move(code)};
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

bool has_only_members(const Json& value, const std::set<std::string>& allowed) {
    if (!value.is_object()) {
        return false;
    }
    for (const auto& member : value.object_range()) {
        if (allowed.count(std::string{member.key()}) == 0) {
            return false;
        }
    }
    return true;
}

bool valid_identifier(std::string_view value) {
    static const std::regex pattern{"^[A-Za-z0-9][A-Za-z0-9._:-]{0,63}$"};
    return std::regex_match(value.begin(), value.end(), pattern);
}

bool finite_number(const Json& value) {
    return (value.is_double() || value.is_int64() || value.is_uint64()) &&
           std::isfinite(value.as<double>());
}

bool integer_one(const Json& value) {
    return (value.is_int64() && value.as<std::int64_t>() == 1) ||
           (value.is_uint64() && value.as<std::uint64_t>() == 1U);
}

}  // namespace

Result<RoadNetworkSummary> validate_road_network_v1(
    std::string_view json_text,
    std::string_view expected_coordinate_mode) {
    if (!detail::bounded_road_document(json_text,kMaximumDocumentBytes)) {
        return invalid("invalid_network", "road-network document is empty");
    }
    try {
        const auto network = Json::parse(json_text);
        static const std::set<std::string> root_fields{
            "schema_version", "coordinate_mode", "nodes", "edges"};
        if (!has_exact_members(network, root_fields)) {
            return invalid("unknown_field",
                           "road-network root fields are invalid");
        }
        if (!integer_one(network.at("schema_version"))) {
            return invalid("unsupported_schema", "schema_version must be 1");
        }
        if (!network.at("coordinate_mode").is_string()) {
            return invalid("invalid_coordinate_mode",
                           "coordinate_mode must be a string");
        }
        const auto coordinate_mode =
            network.at("coordinate_mode").as<std::string>();
        if (coordinate_mode != "metric" &&
            coordinate_mode != "legacy-normalized") {
            return invalid("invalid_coordinate_mode",
                           "coordinate_mode is unsupported");
        }
        if (!expected_coordinate_mode.empty() &&
            coordinate_mode != expected_coordinate_mode) {
            return invalid("coordinate_mode_mismatch",
                           "road-network coordinate_mode does not match the map");
        }
        if (!network.at("nodes").is_array() ||
            !network.at("edges").is_array()) {
            return invalid("invalid_network", "nodes and edges must be arrays");
        }
        const auto& nodes = network.at("nodes");
        const auto& edges = network.at("edges");
        if (nodes.size() > kMaximumNodes || edges.size() > kMaximumEdges) {
            return invalid("network_too_large",
                           "road-network exceeds the node or edge limit");
        }

        std::unordered_set<std::string> node_ids;
        std::vector<Json> normalized_nodes;
        normalized_nodes.reserve(nodes.size());
        static const std::set<std::string> node_fields{
            "id", "x", "y", "kind", "label"};
        for (const auto& node : nodes.array_range()) {
            if (!has_exact_members(node, node_fields) ||
                !node.at("id").is_string() ||
                !node.at("kind").is_string() ||
                !node.at("label").is_string()) {
                return invalid("invalid_node", "road-network node shape is invalid");
            }
            const auto id = node.at("id").as<std::string>();
            const auto label = node.at("label").as<std::string>();
            if (!valid_identifier(id) ||
                node.at("kind").as<std::string>() != "waypoint" ||
                !finite_number(node.at("x")) ||
                !finite_number(node.at("y"))) {
                return invalid("invalid_node", "road-network node value is invalid");
            }
            if (!node_ids.insert(id).second) {
                return invalid("duplicate_node", "road-network node id is duplicated");
            }
            Json normalized(jsoncons::json_object_arg);
            normalized["id"] = id;
            normalized["x"] = node.at("x").as<double>();
            normalized["y"] = node.at("y").as<double>();
            normalized["kind"] = "waypoint";
            normalized["label"] = label;
            normalized_nodes.push_back(std::move(normalized));
        }

        std::unordered_set<std::string> edge_ids;
        std::unordered_set<std::string> edge_keys;
        std::vector<Json> normalized_edges;
        normalized_edges.reserve(edges.size());
        static const std::set<std::string> edge_fields{
            "id", "from", "to", "direction", "max_speed_mps"};
        for (const auto& edge : edges.array_range()) {
            if (!has_only_members(edge, edge_fields) || edge.size() < 4 ||
                !edge.contains("id") || !edge.contains("from") ||
                !edge.contains("to") || !edge.contains("direction") ||
                !edge.at("id").is_string() || !edge.at("from").is_string() ||
                !edge.at("to").is_string() || !edge.at("direction").is_string()) {
                return invalid("invalid_edge", "road-network edge shape is invalid");
            }
            const auto id = edge.at("id").as<std::string>();
            const auto from = edge.at("from").as<std::string>();
            const auto to = edge.at("to").as<std::string>();
            const auto direction = edge.at("direction").as<std::string>();
            if (!valid_identifier(id)) {
                return invalid("invalid_edge", "road-network edge id is invalid");
            }
            if (!edge_ids.insert(id).second) {
                return invalid("duplicate_edge", "road-network edge id is duplicated");
            }
            if (node_ids.count(from) == 0 || node_ids.count(to) == 0) {
                return invalid("dangling_edge", "road-network edge references a missing node");
            }
            if (from == to) {
                return invalid("self_loop", "road-network edge is a self-loop");
            }
            if (direction != "both" && direction != "forward") {
                return invalid("invalid_edge", "road-network edge direction is invalid");
            }
            // Separators cannot occur in identifiers as a NUL byte. Reserve
            // each directed arc, including both arcs for a bidirectional edge.
            const auto key = from + std::string(1, '\0') + to;
            const auto reverse_key = to + std::string(1, '\0') + from;
            if (!edge_keys.insert(key).second ||
                (direction == "both" && !edge_keys.insert(reverse_key).second)) {
                return invalid("duplicate_edge",
                               "road-network connection is duplicated");
            }
            if (edge.contains("max_speed_mps") &&
                !edge.at("max_speed_mps").is_null()) {
                if (!finite_number(edge.at("max_speed_mps"))) {
                    return invalid("invalid_edge",
                                   "road-network edge speed is invalid");
                }
                const auto speed = edge.at("max_speed_mps").as<double>();
                if (speed <= 0.0 || speed > kMaximumSpeedMps) {
                    return invalid("invalid_edge",
                                   "road-network edge speed is outside (0, 100]");
                }
            }
            Json normalized(jsoncons::json_object_arg);
            normalized["id"] = id;
            normalized["from"] = from;
            normalized["to"] = to;
            normalized["direction"] = direction;
            if (edge.contains("max_speed_mps") &&
                !edge.at("max_speed_mps").is_null()) {
                normalized["max_speed_mps"] =
                    edge.at("max_speed_mps").as<double>();
            } else {
                normalized["max_speed_mps"] = Json::null();
            }
            normalized_edges.push_back(std::move(normalized));
        }
        const auto by_id = [](const Json& left, const Json& right) {
            return left.at("id").as<std::string>() <
                   right.at("id").as<std::string>();
        };
        std::sort(normalized_nodes.begin(), normalized_nodes.end(), by_id);
        std::sort(normalized_edges.begin(), normalized_edges.end(), by_id);
        Json normalized(jsoncons::json_object_arg);
        normalized["schema_version"] = 1;
        normalized["coordinate_mode"] = coordinate_mode;
        normalized["nodes"] = Json(jsoncons::json_array_arg);
        normalized["edges"] = Json(jsoncons::json_array_arg);
        for (auto& node : normalized_nodes) {
            normalized["nodes"].push_back(std::move(node));
        }
        for (auto& edge : normalized_edges) {
            normalized["edges"].push_back(std::move(edge));
        }
        if (normalized.to_string().size() > kMaximumDocumentBytes) {
            return invalid("network_too_large",
                           "canonical road-network exceeds 10 MiB");
        }
        if (!detail::valid_road_contract(json_text,1))
            return invalid("invalid_network", "road-network violates the frozen schema");
        return RoadNetworkSummary{coordinate_mode, nodes.size(), edges.size()};
    } catch (const std::exception&) {
        return invalid("invalid_network", "road-network JSON is invalid");
    }
}

}  // namespace roc::vehicle
