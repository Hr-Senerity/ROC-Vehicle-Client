#pragma once
#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/file_sink.hpp"
#include <jsoncons/json.hpp>
#include <fstream>

namespace road_test {
using Json=jsoncons::json;
inline constexpr auto task_id="123e4567-e89b-12d3-a456-426614174010";
inline Json fixture(bool both=true,bool curve=true) {
    // Synthetic persisted-output fixture, not a copied SYSTEM production map.
    // Samples intentionally carry distinctive values: the client must not regenerate them.
    auto result=Json::parse(R"({"schema_version":2,"coordinate_mode":"metric",
      "sampling":{"algorithm":"uniform-parameter-v1","spacing":0.5,"precision":6},
      "nodes":[{"id":"b","x":2,"y":0,"kind":"waypoint","label":"B"},
               {"id":"a","x":0,"y":0,"kind":"waypoint","label":"A"}],
      "edges":[{"id":"e","from":"a","to":"b","direction":"both","max_speed_mps":2,
        "geometry":{"type":"cubic_bezier","control1":{"x":0.5,"y":1},"control2":{"x":1.5,"y":1}}}],
      "trajectories":[{"id":"e:forward","edge_id":"e","direction":"forward","max_speed_mps":2,
        "length":2.5,"points":[{"index":0,"x":0,"y":0,"s":0,"heading":1.107149},
        {"index":1,"x":1,"y":0.75,"s":1.25,"heading":-0.643501},
        {"index":2,"x":2,"y":0,"s":2.5,"heading":-0.643501}]},
        {"id":"e:reverse","edge_id":"e","direction":"reverse","max_speed_mps":2,"length":2.5,
        "points":[{"index":0,"x":2,"y":0,"s":0,"heading":2.498092},
        {"index":1,"x":1,"y":0.75,"s":1.25,"heading":-2.498092},
        {"index":2,"x":0,"y":0,"s":2.5,"heading":-2.498092}]}]})");
    if (!both) {
        result["edges"][0]["direction"]="forward";
        result["trajectories"].erase(result["trajectories"].array_range().begin()+1);
    }
    if (!curve) {
        result["edges"][0]["geometry"]=Json::parse(R"({"type":"line"})");
        for (auto& trajectory:result["trajectories"].array_range()) {
            trajectory["length"]=2;
            for (auto& point:trajectory["points"].array_range()) {
                point["y"]=0; point["s"]=point["index"].as<int>(); point["heading"]=0;
            }
        }
    }
    return result;
}
inline roc::vehicle::ArtifactManifest manifest(std::string_view text,int version=2) {
    roc::vehicle::ArtifactManifest value;
    value.task_id=task_id; value.batch_id="123e4567-e89b-12d3-a456-426614174011";
    value.project_id="123e4567-e89b-12d3-a456-426614174012";
    value.resource_type=roc::vehicle::ResourceType::road_network;
    value.resource_revision_id="123e4567-e89b-12d3-a456-426614174013";
    value.map_id="123e4567-e89b-12d3-a456-426614174014";
    value.schema_version=version; value.version=7;
    value.content_type=version==1 ? "application/vnd.roc.road-network+json" : "application/vnd.roc.road-network.v2+json";
    value.byte_size=text.size(); value.sha256=roc::vehicle::sha256_hex(text).value();
    value.artifact_url=roc::vehicle::task_artifact_target(task_id); return value;
}
inline roc::vehicle::ArtifactHttpMetadata metadata(const roc::vehicle::ArtifactManifest& value) {
    return {value.content_type,value.sha256,value.byte_size,value.byte_size};
}
inline std::string read(const std::filesystem::path& path) {
    std::ifstream input(path,std::ios::binary);
    return {std::istreambuf_iterator<char>{input},std::istreambuf_iterator<char>{}};
}
}
