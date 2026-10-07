#include "roc/vehicle/road_network.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include "contract_validation.hpp"
#include <jsoncons/json.hpp>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>

namespace roc::vehicle {
namespace {
using Json=jsoncons::json;
Error invalid() { return make_resource_failure(ResourceFailure::road_network_invalid); }
double number(const Json& value) {
    const double result=value.as<double>();
    if (!std::isfinite(result)) throw std::invalid_argument("nonfinite");
    return result;
}
RoadPoint position(const Json& value) { return {number(value.at("x")),number(value.at("y"))}; }
std::optional<double> speed(const Json& value) {
    return value.is_null() ? std::optional<double>{} : number(value);
}
bool same(const RoadSample& sample,const RoadPoint& point) { return sample.x==point.x && sample.y==point.y; }
}
Result<RoadNetworkV2> parse_road_network_v2(std::string_view text,RoadValidationOptions options) {
    if (!detail::bounded_road_document(text,options.maximum_document_bytes)) return invalid();
    try {
        const auto document=Json::parse(text,jsoncons::json_options{}.max_nesting_depth(32));
        if (document.is_object() && document.contains("schema_version") &&
            document.at("schema_version")!=Json(2))
            return make_resource_failure(ResourceFailure::unsupported_schema_version);
        if (!detail::valid_road_contract(text,2)) return invalid();
        RoadNetworkV2 result;
        result.coordinate_mode=document.at("coordinate_mode").as<std::string>();
        if (!options.expected_coordinate_mode.empty() && options.expected_coordinate_mode!=result.coordinate_mode)
            return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
        const auto& sampling=document.at("sampling");
        result.sampling={sampling.at("algorithm").as<std::string>(),number(sampling.at("spacing")),
            sampling.at("precision").as<int>()};
        std::map<std::string,RoadPoint> nodes;
        for (const auto& node:document.at("nodes").array_range()) {
            RoadNode parsed{node.at("id").as<std::string>(),position(node),
                node.at("kind").as<std::string>(),node.at("label").as<std::string>()};
            if (!nodes.emplace(parsed.id,parsed.position).second) return invalid();
            result.nodes.push_back(std::move(parsed));
        }
        std::map<std::string,std::size_t> edges;
        std::set<std::pair<std::string,std::string>> arcs;
        std::set<std::pair<std::string,std::string>> expected,seen;
        for (const auto& edge:document.at("edges").array_range()) {
            RoadEdge parsed;
            parsed.id=edge.at("id").as<std::string>(); parsed.from=edge.at("from").as<std::string>();
            parsed.to=edge.at("to").as<std::string>(); parsed.direction=edge.at("direction").as<std::string>();
            parsed.max_speed_mps=speed(edge.at("max_speed_mps"));
            const auto& geometry=edge.at("geometry"); parsed.geometry.type=geometry.at("type").as<std::string>();
            if (parsed.geometry.type=="cubic_bezier") {
                parsed.geometry.control1=position(geometry.at("control1"));
                parsed.geometry.control2=position(geometry.at("control2"));
            }
            if (!edges.emplace(parsed.id,result.edges.size()).second || parsed.from==parsed.to ||
                !nodes.count(parsed.from) || !nodes.count(parsed.to) || !arcs.emplace(parsed.from,parsed.to).second)
                return invalid();
            expected.emplace(parsed.id,"forward");
            if (parsed.direction=="both") {
                if (!arcs.emplace(parsed.to,parsed.from).second) return invalid();
                expected.emplace(parsed.id,"reverse");
            }
            result.edges.push_back(std::move(parsed));
        }
        std::set<std::string> trajectory_ids;
        std::size_t total=0;
        for (const auto& trajectory:document.at("trajectories").array_range()) {
            RoadTrajectory parsed;
            parsed.id=trajectory.at("id").as<std::string>(); parsed.edge_id=trajectory.at("edge_id").as<std::string>();
            parsed.direction=trajectory.at("direction").as<std::string>();
            parsed.max_speed_mps=speed(trajectory.at("max_speed_mps")); parsed.length=number(trajectory.at("length"));
            const auto key=std::make_pair(parsed.edge_id,parsed.direction);
            if (!trajectory_ids.insert(parsed.id).second || !expected.count(key) || !seen.insert(key).second)
                return invalid();
            const auto& edge=result.edges.at(edges.at(parsed.edge_id));
            if (parsed.max_speed_mps!=edge.max_speed_mps) return invalid();
            const auto& points=trajectory.at("points");
            if (points.size()>200000U-total) return invalid();
            total+=points.size(); parsed.points.reserve(points.size());
            for (const auto& point:points.array_range()) {
                const auto index=point.at("index").as<std::uint64_t>();
                if (index!=parsed.points.size()) return invalid();
                RoadSample sample{static_cast<std::size_t>(index),number(point.at("x")),number(point.at("y")),
                    number(point.at("s")),number(point.at("heading"))};
                if (parsed.points.empty() ? sample.s!=0 : sample.s<parsed.points.back().s) return invalid();
                parsed.points.push_back(sample);
            }
            const auto& from=nodes.at(parsed.direction=="forward" ? edge.from : edge.to);
            const auto& to=nodes.at(parsed.direction=="forward" ? edge.to : edge.from);
            if (!same(parsed.points.front(),from) || !same(parsed.points.back(),to) ||
                parsed.points.back().s!=parsed.length) return invalid();
            result.trajectories.push_back(std::move(parsed));
        }
        if (seen!=expected) return invalid();
        return result;
    } catch (const std::exception&) { return invalid(); }
}
}
