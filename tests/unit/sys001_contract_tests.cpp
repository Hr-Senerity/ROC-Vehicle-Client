#include "roc/vehicle/task_protocol.hpp"
#include "roc/vehicle/task_http_client.hpp"
#include "roc/vehicle/road_network.hpp"
#include "roc/vehicle/file_sink.hpp"
#include <jsoncons/json.hpp>
#include <sqlite3.h>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

using namespace roc::vehicle;
using Json = jsoncons::json;
namespace {
int failures = 0;
#define EXPECT(x) do { if (!(x)) { std::cerr << __LINE__ << ": " << #x << '\n'; ++failures; } } while(false)
constexpr auto task_id = "123e4567-e89b-12d3-a456-426614174010";
constexpr auto batch_id = "123e4567-e89b-12d3-a456-426614174011";
constexpr auto project_id = "123e4567-e89b-12d3-a456-426614174012";
constexpr auto revision_id = "123e4567-e89b-12d3-a456-426614174013";

Json task() {
    return Json::parse(R"({
        "id":"123e4567-e89b-12d3-a456-426614174010",
        "batch_id":"123e4567-e89b-12d3-a456-426614174011",
        "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
        "state":"accepted","attempt":1,"max_attempts":3,"progress":20,
        "error_code":null,"error_message":null,"offered_at":null,
        "accepted_at":null,"delivered_at":null,"updated_at":"2026-10-04T00:00:00Z"
    })");
}
TaskAvailableMessage notification() {
    TaskAvailableMessage n;
    n.task_id = task_id; n.batch_id = batch_id; n.project_id = project_id;
    n.resource_revision_id = revision_id; n.resource_type = ResourceType::map;
    n.max_attempts = 3;
    return n;
}
void responses() {
    Json response = Json::parse(R"({"ok":true,"replayed":false,
        "lease_token":"synthetic-only-lease","lease_expires_at":4102444800})");
    response["task"] = task();
    EXPECT(parse_accept_response(response.to_string(), task_id));
    auto additional = response;
    additional["future_optional"] = true;
    additional["task"]["future_optional"] = true;
    EXPECT(parse_accept_response(additional.to_string(), task_id));
    for (const auto* field : {"batch_id", "vehicle_id", "state", "attempt",
            "max_attempts", "progress", "error_code", "error_message", "offered_at",
            "accepted_at", "delivered_at", "updated_at"}) {
        auto missing = response;
        missing["task"].erase(field);
        EXPECT(!parse_accept_response(missing.to_string(), task_id));
    }
    auto invalid = response;
    invalid["task"]["updated_at"] = "2026-02-30T00:00:00Z";
    EXPECT(!parse_accept_response(invalid.to_string(), task_id));
    invalid = response; invalid["lease_token"] = "short";
    EXPECT(!parse_accept_response(invalid.to_string(), task_id));
    invalid = response; invalid["task"]["state"] = "downloading";
    EXPECT(!parse_accept_response(invalid.to_string(), task_id));
    Json status(jsoncons::json_object_arg);
    status["ok"] = true; status["replayed"] = true; status["project_id"] = project_id;
    status["task"] = task();
    status["task"]["state"] = "delivered"; status["task"]["attempt"] = 2;
    status["task"]["progress"] = 100;
    EXPECT(parse_status_response(status.to_string(), task_id));
    additional = status; additional["future_optional"] = true;
    EXPECT(parse_status_response(additional.to_string(), task_id));
    status["task"]["id"] = batch_id;
    EXPECT(!parse_status_response(status.to_string(), task_id));
    status["task"] = task(); status.erase("project_id");
    EXPECT(!parse_status_response(status.to_string(), task_id));
}
void errors() {
    for (const auto& group : std::vector<std::pair<unsigned, std::vector<std::string>>>{
        {400, {"invalid_id","invalid_json","unknown_field","invalid_status_state",
               "invalid_progress","invalid_error_code","invalid_error_message","error_code_required"}},
        {401, {"authentication_failed","invalid_lease"}},
        {404, {"task_not_found","artifact_not_found"}},
        {409, {"invalid_state","lease_expired","lease_conflict","attempts_exhausted",
               "event_conflict","invalid_transition","file_id_required"}},
        {416, {"range_not_supported"}}, {500, {"internal_error"}}}) {
        for (const auto& code : group.second) {
            Json body(jsoncons::json_object_arg);
            body["ok"] = false; body["code"] = code; body["message"] = "synthetic";
            EXPECT(parse_http_error_response(body.to_string(), group.first, true));
            EXPECT(!parse_http_error_response(body.to_string(), 418, true));
            body["unknown"] = true;
            EXPECT(!parse_http_error_response(body.to_string(), group.first, true));
        }
    }
    EXPECT(!parse_http_error_response(R"({"ok":false,"code":"future","message":"x"})",409));
    EXPECT(!parse_http_error_response(R"({"ok":false,"code":"invalid_lease","message":"x"})",400));
    EXPECT(!parse_http_error_response(R"({"ok":false,"code":"range_not_supported","message":"x"})",416));
}
void package_manifest() {
    Json manifest = Json::parse(R"({
        "task_id":"123e4567-e89b-12d3-a456-426614174010",
        "batch_id":"123e4567-e89b-12d3-a456-426614174011",
        "project_id":"123e4567-e89b-12d3-a456-426614174012",
        "resource_type":"map","resource_revision_id":"123e4567-e89b-12d3-a456-426614174013",
        "map_id":"123e4567-e89b-12d3-a456-426614174014",
        "content_type":"application/vnd.roc.map-package+json","byte_size":4,
        "version":2,"package_version":2,"package_digest_algorithm":"roc-file-set-v1",
        "map_format":"pgm-yaml","coordinate_mode":"metric","resolution":0.05,
        "origin":{"x":0,"y":0,"theta":0},
        "files":[]
    })");
    std::vector<ArtifactFile> files;
    for (const auto* role : {"pgm","yaml"}) {
        ArtifactFile file;
        file.id = role == std::string{"pgm"} ? batch_id : revision_id;
        file.role = role; file.file_name = std::string{role} + ".original";
        file.content_type = "application/octet-stream"; file.byte_size = 2;
        file.sha256 = std::string(64, role == std::string{"pgm"} ? 'a' : 'b');
        file.artifact_url = task_artifact_file_target(task_id, file.id);
        files.push_back(file);
        Json item(jsoncons::json_object_arg);
        item["id"]=file.id; item["role"]=file.role; item["file_name"]=file.file_name;
        item["content_type"]=file.content_type; item["byte_size"]=file.byte_size;
        item["sha256"]=file.sha256; item["artifact_url"]=file.artifact_url;
        manifest["files"].push_back(item);
    }
    manifest["sha256"] = map_package_digest(files).value();
    Json body(jsoncons::json_object_arg); body["ok"]=true; body["manifest"]=manifest;
    const auto parse = [&](const Json& value) {
        return parse_manifest_response(value.to_string(), notification(), ArtifactLimits{});
    };
    const auto parsed = parse(body);
    EXPECT(parsed);
    if (parsed) EXPECT(!FileSink::create(std::filesystem::temp_directory_path()/
        "roc-vehicle-package-must-not-use-single-sink", parsed.value()));
    std::swap(files[0], files[1]);
    EXPECT(map_package_digest(files).value() == manifest["sha256"].as<std::string>());
    for (const int version : {1,3}) {
        auto invalid=body; invalid["manifest"]["package_version"]=version;
        EXPECT(!parse(invalid));
    }
    auto invalid=body; invalid["manifest"]["files"][1]["role"]="pgm";
    EXPECT(!parse(invalid));
    invalid=body; invalid["manifest"]["files"][1]["id"]=batch_id;
    EXPECT(!parse(invalid));
    invalid=body; invalid["manifest"]["byte_size"]=5;
    EXPECT(!parse(invalid));
    invalid=body; invalid["manifest"]["sha256"]=std::string(64,'c');
    EXPECT(!parse(invalid));
    invalid=body; invalid["manifest"]["files"][0]["artifact_url"]="/api/device/tasks/escape";
    EXPECT(!parse(invalid));
    invalid=body; invalid["manifest"]["artifact_url"]=task_artifact_target(task_id);
    EXPECT(!parse(invalid));
}
void active_recovery() {
    const auto root = std::filesystem::temp_directory_path()/ "roc-vehicle-sys001-active";
    std::error_code ec; std::filesystem::remove_all(root, ec);
    auto store = std::move(DurableStateStore::open(root/"state"/"state.sqlite3").value());
    EXPECT(store->register_task_generation({task_id,1},20));
    ActiveTaskLease lease{notification(),{task_id,1},"synthetic-only-lease",4102444800,20};
    EXPECT(store->persist_active_task_lease(lease));
    auto changed=lease; changed.lease_token="different-synthetic-lease";
    EXPECT(!store->persist_active_task_lease(changed));
    StatusEventRecord event;
    event.event_id=batch_id; event.generation={task_id,1};
    event.state=TaskExecutionState::downloading; event.progress=30;
    event.lease_token=lease.lease_token; event.lease_expires_at=lease.lease_expires_at;
    event.local_delivery_evidence=R"({"phase":"download"})";
    event.payload_json=make_status_request(event).value();
    EXPECT(store->persist_status_event(event));
    EXPECT(store->acknowledge_status_event(event.event_id));
    EXPECT(store->status_event(event.event_id).value().lease_token.empty());
    store.reset();
    store=std::move(DurableStateStore::open(root/"state"/"state.sqlite3").value());
    ClientConfig config;
    config.base_url="http://127.0.0.1:1"; config.state_directory=root/"state";
    config.artifact_directory=root/"artifacts";
    auto client=std::move(DeviceTaskHttpClient::create(config, [] {
        return Result<std::string>{"synthetic-only-token"}; }).value());
    auto recovered=client->recover_accepted_task({task_id,1},*store);
    EXPECT(recovered);
    if(recovered) {
        EXPECT(recovered.value().lease_token==lease.lease_token);
        EXPECT(recovered.value().progress_floor==30);
    }
    auto blocked=client->accept_task(notification(),*store);
    EXPECT(!blocked);
    EXPECT(blocked.error().server_code=="active_task_requires_recovery");
    EXPECT(store->mark_task_generation_expired({task_id,1}));
    EXPECT(!store->active_task_lease({task_id,1}));
    store.reset(); std::filesystem::remove_all(root, ec);
}
void v2_migration_preserves_replay(bool terminal) {
    const auto root = std::filesystem::temp_directory_path()/
        (terminal ? "roc-vehicle-sys001-v2-terminal" : "roc-vehicle-sys001-v2-active");
    std::error_code ec; std::filesystem::remove_all(root, ec);
    const auto database = root/"state"/"state.sqlite3";
    auto store = std::move(DurableStateStore::open(database).value());
    EXPECT(store->register_task_generation({task_id,1},20));
    StatusEventRecord event;
    event.event_id = batch_id; event.generation = {task_id,1};
    event.state = terminal ? TaskExecutionState::failed : TaskExecutionState::downloading;
    event.progress = 20;
    if (terminal) event.error_code = "local_storage_failure";
    event.lease_token = "synthetic-only-lease"; event.lease_expires_at = 1;
    event.local_delivery_evidence = R"({"phase":"failed"})";
    event.payload_json = make_status_request(event).value();
    EXPECT(store->persist_status_event(event));
    const auto before = store->status_event(event.event_id).value();
    const auto reserved = store->allocate_sequence().value();
    store.reset();
    // Generate an exact schema-v2 fixture by removing only the v3 additions.
    sqlite3* raw = nullptr;
    EXPECT(sqlite3_open(database.string().c_str(), &raw) == SQLITE_OK);
    EXPECT(sqlite3_exec(raw,
        "DROP TRIGGER clear_active_terminal_lease;"
        "DROP TRIGGER clear_expired_active_lease;"
        "DROP TABLE active_task_leases;"
        "DELETE FROM schema_migrations WHERE version = 3;",
        nullptr, nullptr, nullptr) == SQLITE_OK);
    EXPECT(sqlite3_close(raw) == SQLITE_OK);
    store = std::move(DurableStateStore::open(database).value());
    const auto after = store->status_event(event.event_id).value();
    EXPECT(after.disposition == StatusEventDisposition::pending);
    EXPECT(after.payload_json == before.payload_json);
    EXPECT(after.request_sha256 == before.request_sha256);
    EXPECT(after.lease_token == before.lease_token);
    EXPECT(after.lease_expires_at == before.lease_expires_at);
    EXPECT(after.local_delivery_evidence == before.local_delivery_evidence);
    EXPECT(store->allocate_sequence().value() > reserved);
    EXPECT(store->active_task_keys().value().empty());
    EXPECT(store->has_unconfirmed_terminal_event(task_id).value()==terminal);
    EXPECT(store->has_unrecoverable_active_generation(task_id).value()==!terminal);
    if (!terminal) {
        ClientConfig config; config.base_url="http://127.0.0.1:1";
        config.state_directory=root/"state"; config.artifact_directory=root/"artifacts";
        auto client=std::move(DeviceTaskHttpClient::create(config, [] {
            return Result<std::string>{"synthetic-only-token"}; }).value());
        const auto blocked=client->accept_task(notification(),*store);
        EXPECT(!blocked); EXPECT(blocked.error().server_code=="active_task_recovery_missing");
    }
    store.reset(); std::filesystem::remove_all(root, ec);
}
void active_terminal_cleanup(bool resolve) {
    const auto root = std::filesystem::temp_directory_path()/
        (resolve ? "roc-vehicle-sys001-resolve" : "roc-vehicle-sys001-ack");
    std::error_code ec; std::filesystem::remove_all(root, ec);
    const auto database = root/"state"/"state.sqlite3";
    auto store = std::move(DurableStateStore::open(database).value());
    EXPECT(store->register_task_generation({task_id,1},20));
    EXPECT(store->persist_active_task_lease(
        {notification(),{task_id,1},"synthetic-only-lease",4102444800,20}));
    StatusEventRecord event;
    event.event_id = batch_id; event.generation = {task_id,1};
    event.state = TaskExecutionState::failed; event.progress = 20;
    event.error_code = "local_storage_failure";
    event.lease_token = "synthetic-only-lease"; event.lease_expires_at = 4102444800;
    event.local_delivery_evidence = R"({"phase":"failed"})";
    event.payload_json = make_status_request(event).value();
    EXPECT(store->persist_status_event(event));
    if (resolve) {
        EXPECT(store->mark_status_event_reconciliation_required(event.event_id,"event_conflict"));
        EXPECT(store->resolve_status_event_reconciliation(event.event_id));
    } else {
        EXPECT(store->acknowledge_status_event(event.event_id));
    }
    EXPECT(store->status_event(event.event_id).value().lease_token.empty());
    store.reset();
    sqlite3* raw = nullptr; sqlite3_stmt* query = nullptr;
    EXPECT(sqlite3_open_v2(database.string().c_str(),&raw,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
    EXPECT(sqlite3_prepare_v2(raw,"SELECT COUNT(*) FROM active_task_leases",-1,&query,nullptr)==SQLITE_OK);
    EXPECT(sqlite3_step(query)==SQLITE_ROW);
    EXPECT(sqlite3_column_int(query,0)==0);
    sqlite3_finalize(query); sqlite3_close(raw);
    store = std::move(DurableStateStore::open(database).value());
    EXPECT(!store->has_unconfirmed_terminal_event(task_id).value());
    store.reset(); std::filesystem::remove_all(root, ec);
}
void directed_arcs() {
    const auto network=R"({"schema_version":1,"coordinate_mode":"metric",
        "nodes":[{"id":"A","x":0,"y":0,"kind":"waypoint","label":""},
                 {"id":"B","x":1,"y":1,"kind":"waypoint","label":""}],
        "edges":[{"id":"e1","from":"A","to":"B","direction":"both"},
                 {"id":"e2","from":"B","to":"A","direction":"forward"}]})";
    EXPECT(!validate_road_network_v1(network));
}
}
int main() {
    responses(); errors(); package_manifest(); active_recovery(); directed_arcs();
    v2_migration_preserves_replay(true); v2_migration_preserves_replay(false);
    active_terminal_cleanup(false); active_terminal_cleanup(true);
    return failures==0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
