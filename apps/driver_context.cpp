#include "driver_support.hpp"
#include "private_paths.hpp"
#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/protocol.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include <jsoncons/json.hpp>
#include <algorithm>
#include <set>

namespace roc::vehicle::driver {
namespace {
using Json = jsoncons::json;
constexpr auto system_commit = "a9c601d6977d89000438854a24e3199573004a2f";
bool closed(const Json& j, const std::set<std::string>& keys) {
    if (!j.is_object() || j.size()!=keys.size()) return false;
    for (const auto& field:j.object_range()) if (!keys.count(std::string(field.key()))) return false;
    return true;
}
Json document(const OwningMapContext& c) {
    Json j(jsoncons::json_object_arg);
    j["project_id"]=c.project_id; j["resource_revision_id"]=c.resource_revision_id;
    j["artifact_sha256"]=c.artifact_sha256; j["map_id"]=c.map_id;
    j["map_revision_id"]=c.map_revision_id; j["coordinate_mode"]=c.coordinate_mode;
    return j;
}
}
Result<std::vector<OwningMapContext>> read_owning_maps(const std::filesystem::path& path, std::string_view vehicle_id) {
    if (!detail::private_directory(path.parent_path(),false)) return local_error("owning_map_context_invalid");
    const auto bytes=read_private_file(path,65536);
    if (!bytes) return local_error("owning_map_context_invalid");
    try {
        jsoncons::json_options options; options.max_nesting_depth(32);
        const auto j=Json::parse(bytes.value(),options);
        if (!closed(j,{"context_version","system_commit","vehicle_id","bindings"}) ||
            !j["context_version"].is_int64() || j["context_version"].as<std::int64_t>()!=1 ||
            j["system_commit"].as<std::string>()!=system_commit ||
            j["vehicle_id"].as<std::string>()!=vehicle_id || !j["bindings"].is_array() ||
            j["bindings"].empty() || j["bindings"].size()>128) return local_error("owning_map_context_invalid");
        std::vector<OwningMapContext> result; std::set<std::string> revisions;
        for (const auto& row:j["bindings"].array_range()) {
            if (!closed(row,{"project_id","resource_revision_id","artifact_sha256","map_id","map_revision_id","coordinate_mode"}))
                return local_error("owning_map_context_invalid");
            for (const auto& field:row.object_range()) if (!field.value().is_string()) return local_error("owning_map_context_invalid");
            OwningMapContext c{row["project_id"].as<std::string>(),row["resource_revision_id"].as<std::string>(),
                row["artifact_sha256"].as<std::string>(),row["map_id"].as<std::string>(),
                row["map_revision_id"].as<std::string>(),row["coordinate_mode"].as<std::string>()};
            if (!is_valid_uuid(c.project_id) || !is_valid_uuid(c.resource_revision_id) ||
                !is_valid_uuid(c.map_id) || !is_valid_uuid(c.map_revision_id) ||
                c.artifact_sha256.size()!=64 || !std::all_of(c.artifact_sha256.begin(),c.artifact_sha256.end(),[](char ch) {
                    return (ch>='0' && ch<='9') || (ch>='a' && ch<='f');
                }) || (c.coordinate_mode!="metric" && c.coordinate_mode!="legacy-normalized") ||
                !revisions.insert(c.resource_revision_id).second) return local_error("owning_map_context_invalid");
            result.push_back(std::move(c));
        }
        return result;
    } catch (...) { return local_error("owning_map_context_invalid"); }
}
Result<std::string> bind_owning_map(const Settings& s,const ArtifactManifest& m,const TaskGenerationKey& key) {
    if (m.resource_type!=ResourceType::road_network) return std::string{};
    if (!s.owning_maps_configured) {
        if (s.remote) return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
        return std::string{}; // Explicitly unverified compatibility fixtures, never a server acceptance claim.
    }
    const auto it=std::find_if(s.owning_maps.begin(),s.owning_maps.end(),[&](const auto& c) {
        return c.resource_revision_id==m.resource_revision_id;
    });
    const auto path=s.client.state_directory / ("driver-map-context-"+key.task_id+"-"+std::to_string(key.attempt)+".json");
    std::error_code ec;
    const bool existing=std::filesystem::exists(std::filesystem::symlink_status(path,ec));
    if (ec && ec!=std::errc::no_such_file_or_directory) return local_error("journal_conflict",ErrorDomain::storage);
    if (it==s.owning_maps.end()) {
        if (existing) return local_error("journal_conflict",ErrorDomain::storage);
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    }
    auto j=document(*it); j["context_version"]=1; j["system_commit"]=system_commit;
    j["vehicle_id"]=s.client.expected_vehicle_id;
    // An existing generation's trusted input takes precedence over a NEW
    // mismatch classification: never emit failed to conceal an already saved final.
    if (existing) {
        const auto previous=read_private_file(path,65536);
        if (!previous || previous.value()!=j.to_string()) return local_error("journal_conflict",ErrorDomain::storage);
    }
    if (it->project_id!=m.project_id || it->map_id!=m.map_id || it->artifact_sha256!=m.sha256)
        return make_resource_failure(ResourceFailure::artifact_metadata_mismatch);
    const auto saved=save_immutable(path,j.to_string());
    if (!saved) return saved.error(); // Changed trust input cannot retroactively validate a published generation.
    return it->coordinate_mode;
}
}
