#include "../road_test_support.hpp"
#include "roc/vehicle/road_receiver.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdlib>
#include <iostream>
#include <thread>

using namespace roc::vehicle;
using namespace road_test;
namespace asio=boost::asio;namespace beast=boost::beast;namespace http=beast::http;
using tcp=asio::ip::tcp;
namespace {
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n';++failures; } } while(false)
Json snapshot(std::string state,int progress) {
    auto j=Json::parse(R"({"id":"123e4567-e89b-12d3-a456-426614174010",
      "batch_id":"123e4567-e89b-12d3-a456-426614174011",
      "vehicle_id":"123e4567-e89b-12d3-a456-426614174001","state":"accepted",
      "attempt":1,"max_attempts":3,"progress":30,"error_code":null,"error_message":null,
      "offered_at":null,"accepted_at":null,"delivered_at":null,"updated_at":"2026-10-04T00:00:00Z"})");
    j["state"]=state;j["progress"]=progress;return j;
}
Json manifest_response(const ArtifactManifest& m) {
    Json j(jsoncons::json_object_arg);j["ok"]=true;Json x(jsoncons::json_object_arg);
    x["task_id"]=m.task_id;x["batch_id"]=m.batch_id;x["project_id"]=m.project_id;x["resource_type"]="road_network";
    x["resource_revision_id"]=m.resource_revision_id;x["map_id"]=m.map_id;x["content_type"]=m.content_type;
    x["byte_size"]=m.byte_size;x["sha256"]=m.sha256;x["version"]=m.version;x["schema_version"]=m.schema_version;
    x["artifact_url"]=m.artifact_url;j["manifest"]=x;return j;
}
void flow(const std::filesystem::path& root,int mode) {
    auto original=fixture();auto wire=original;
    if (mode==3) wire["trajectories"][0]["points"][1]["index"]=0;
    const auto text=wire.to_string();const auto m=manifest(text);auto body=manifest_response(m);
    if (mode==1) body["manifest"]["resource_revision_id"]="123e4567-e89b-12d3-a456-426614174099";
    if (mode==5) body["manifest"]["schema_version"]=3;
    asio::io_context context;tcp::acceptor acceptor(context,{asio::ip::address_v4::loopback(),0});
    bool server_ok=true;std::string terminal_body,terminal_lease;
    const int count=mode==0?7:(mode==1 || mode==5?3:5);
    std::thread server([&] {
        for (int i=0;i<count;++i) {
            boost::system::error_code ec;tcp::socket socket(context);acceptor.accept(socket,ec);
            beast::flat_buffer buffer;http::request<http::string_body> request;http::read(socket,buffer,request,ec);
            if (ec) {server_ok=false;return;}
            const auto target=i==0?task_accept_target(task_id):i==1?task_manifest_target(task_id):
                i==3?task_artifact_target(task_id):task_status_target(task_id);
            if (request.target()!=target || request[http::field::authorization]!="Device synthetic-road-token" ||
                (i!=0 && request["X-Task-Lease"]!="synthetic-road-lease") || !request[http::field::range].empty())server_ok=false;
            if (i==5) {terminal_body=request.body();terminal_lease=std::string{request["X-Task-Lease"]};socket.close(ec);continue;}
            http::response<http::string_body> response(http::status::ok,11);response.keep_alive(false);
            response.set(http::field::content_type,"application/json");
            if (i==0) {
                Json accept(jsoncons::json_object_arg);accept["ok"]=true;accept["replayed"]=false;
                accept["lease_token"]="synthetic-road-lease";accept["lease_expires_at"]=4102444800LL;
                accept["task"]=snapshot("accepted",30);response.body()=accept.to_string();
            } else if (i==1) response.body()=body.to_string();
            else if (i==3) {
                response.set(http::field::content_type,mode==4?"application/json":m.content_type);
                response.set("X-Content-SHA256",m.sha256);response.body()=text;
                if (mode==2) response.body()[response.body().find("0.75")]='9';
            } else {
                Json status(jsoncons::json_object_arg);status["ok"]=true;status["replayed"]=i==6;
                const bool failed=mode!=0 && i==count-1;
                const auto state=failed?"failed":i==2?"downloading":i==4?"delivering":"delivered";
                status["project_id"]=m.project_id;status["task"]=snapshot(state,failed?30:i==2?30:i==4?75:100);
                const auto sent=Json::parse(request.body());
                if (sent["state"].as<std::string>()!=state)server_ok=false;
                if (failed) {
                    if (sent["error_code"].as<std::string>().empty())server_ok=false;
                    status["task"]["error_code"]=sent["error_code"];status["task"]["error_message"]="resource operation failed";
                }
                if (i==6) {
                    if (request.body()!=terminal_body || request["X-Task-Lease"]!=terminal_lease)server_ok=false;
                    status["task"]["attempt"]=2;
                }
                response.body()=status.to_string();
            }
            response.prepare_payload();http::write(socket,response,ec);
        }
    });
    ClientConfig config;config.base_url="http://127.0.0.1:"+std::to_string(acceptor.local_endpoint().port());
    config.state_directory=root/"state";config.artifact_directory=root/"artifacts";config.artifact_limits.stream_chunk_bytes=7;
    auto client=DeviceTaskHttpClient::create(config,[]{return Result<std::string>{"synthetic-road-token"};});
    auto store=DurableStateStore::open(config.state_directory/"state.sqlite3");EXPECT(client && store);if (!client || !store)std::abort();
    TaskAvailableMessage notice;notice.task_id=task_id;notice.batch_id=m.batch_id;notice.project_id=m.project_id;
    notice.resource_type=ResourceType::road_network;notice.resource_revision_id=m.resource_revision_id;notice.max_attempts=3;
    auto accepted=client.value()->accept_task(notice,*store.value());EXPECT(accepted);
    if (!accepted) {std::cerr<<accepted.error().server_code<<": "<<accepted.error().message<<'\n';std::abort();}
    EXPECT(accepted.value().progress_floor==30);
    const auto send_failed=[&](const Error& error) {
        StatusEventRecord failed;failed.generation={task_id,1};failed.state=TaskExecutionState::failed;
        failed.progress=30;failed.error_code=error.server_code;failed.error_message="resource operation failed";
        failed.lease_token=accepted.value().lease_token;failed.lease_expires_at=accepted.value().lease_expires_at;
        failed.event_id=generate_uuid_v4().value();failed.local_delivery_evidence=R"({"verified":false,"committed":false})";
        failed.payload_json=make_status_request(failed).value();EXPECT(store.value()->persist_status_event(failed));
        EXPECT(client.value()->report_status(failed,accepted.value(),*store.value()));
        EXPECT(!store.value()->has_unconfirmed_terminal_event(task_id).value());
    };
    auto fetched=client.value()->read_manifest(notice,accepted.value(),*store.value());
    if (mode==1 || mode==5) {
        EXPECT(!fetched);if (!fetched) EXPECT(fetched.error().server_code==(mode==1?"artifact_metadata_mismatch":"unsupported_schema_version"));
        if (!fetched) send_failed(fetched.error());
    } else {
        EXPECT(fetched);if (!fetched)std::abort();
        StatusEventRecord event;event.generation={task_id,1};event.lease_token=accepted.value().lease_token;
        event.lease_expires_at=accepted.value().lease_expires_at;event.progress=30;event.state=TaskExecutionState::downloading;
        event.local_delivery_evidence=R"({"phase":"download"})";
        const auto send=[&]() {
            event.event_id=generate_uuid_v4().value();event.payload_json=make_status_request(event).value();
            EXPECT(store.value()->persist_status_event(event));return client.value()->report_status(event,accepted.value(),*store.value());
        };
        EXPECT(send());auto sink=FileSink::create(config.artifact_directory,fetched.value(),{"metric"});EXPECT(sink);if (!sink)std::abort();
        auto download=client.value()->download_artifact(fetched.value(),accepted.value(),*store.value(),
            [&](const std::uint8_t* data,std::size_t size){return sink.value()->write(data,size);});EXPECT(download);if (!download)std::abort();
        auto sealed=sink.value()->seal(download.value());
        if (mode!=0) {
            EXPECT(!sealed);EXPECT(!sink.value()->commit());
            if (!sealed) EXPECT(sealed.error().server_code==(mode==2?"artifact_hash_mismatch":mode==3?"road_network_invalid":"artifact_metadata_mismatch"));
            EXPECT(!store.value()->has_unconfirmed_terminal_event(task_id).value());
            if (!sealed) send_failed(sealed.error());
        } else {
            EXPECT(sealed);event.state=TaskExecutionState::delivering;event.progress=75;EXPECT(send());
            auto delivered=sink.value()->commit();EXPECT(delivered);if (!delivered)std::abort();
            bool received=false;
            EXPECT(deliver_road_network_v2_to_callback(delivered.value(),[&](const DeliveredRoadNetworkV2& value)->Result<void>{
                received=true;EXPECT(value.network.trajectories[0].points[1].y==0.75);
                EXPECT(value.network.trajectories[1].points[0].x==2);EXPECT(read(value.artifact.path)==text);return {};
            },{"metric"}));EXPECT(received);
            Json evidence(jsoncons::json_object_arg);evidence["receiver"]="verified_file";
            evidence["schema_version"]=2;evidence["task_id"]=task_id;evidence["attempt"]=1;
            evidence["resource_revision_id"]=m.resource_revision_id;evidence["sha256"]=m.sha256;
            evidence["byte_size"]=m.byte_size;evidence["name"]=delivered.value().path.filename().string();
            event.local_delivery_evidence=evidence.to_string();event.state=TaskExecutionState::delivered;event.progress=100;
            EXPECT(!send());EXPECT(store.value()->has_unconfirmed_terminal_event(task_id).value());
            const auto original_request=event.payload_json;store.value().reset();store=DurableStateStore::open(config.state_directory/"state.sqlite3");
            auto retained=store.value()->status_event(event.event_id);EXPECT(retained);
            EXPECT(retained.value().payload_json==original_request && retained.value().lease_token==event.lease_token);
            EXPECT(retained.value().local_delivery_evidence==event.local_delivery_evidence);
            EXPECT(client.value()->replay_status_event(retained.value(),*store.value()));
            EXPECT(!store.value()->has_unconfirmed_terminal_event(task_id).value());
            EXPECT(load_verified_road_network_v2(delivered.value(),{"metric"}));
        }
    }
    server.join();EXPECT(server_ok);
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/"roc-road-http";
    std::error_code ec;std::filesystem::remove_all(root,ec);
    for (int i=0;i<=5;++i)flow(root/std::to_string(i),i);
    std::filesystem::remove_all(root,ec);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
