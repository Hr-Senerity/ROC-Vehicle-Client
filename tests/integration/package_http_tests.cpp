#include "../package_test_support.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <thread>
using namespace roc::vehicle;
using namespace package_test;
namespace asio=boost::asio; namespace beast=boost::beast; namespace http=beast::http;
using tcp=asio::ip::tcp;
namespace {
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n'; ++failures; } } while(false)
void flow(const std::filesystem::path& root,int mode) {
    auto value=fixture();
    auto original=prepared(root/"synthetic-source",value); EXPECT(original); if (!original) return;
    const auto manifest_body=read(original.value()->staging_path()/"manifest.json"); original.value().reset();
    asio::io_context context; tcp::acceptor acceptor(context,{asio::ip::address_v4::loopback(),0});
    bool server_ok=true; std::string original_request,original_lease;
    std::thread server([&] {
        for (int index=0;index<(mode==0?5:3);++index) {
            boost::system::error_code error; tcp::socket socket(context); acceptor.accept(socket,error);
            beast::flat_buffer buffer; http::request<http::string_body> request;
            http::read(socket,buffer,request,error);
            if (error) { server_ok=false; return; }
            const std::string target=index==0 ? task_manifest_target(task_id) :
                (index<=2 ? value.manifest.files[static_cast<std::size_t>(index-1)].artifact_url : task_status_target(task_id));
            if (request.target()!=target || request[http::field::authorization]!="Device synthetic-package-token" ||
                request["X-Task-Lease"]!="synthetic-package-lease" || !request[http::field::range].empty() ||
                request.method()!=(index>=3 ? http::verb::post : http::verb::get)) server_ok=false;
            if (index==3) {
                original_request=request.body(); original_lease=std::string{request["X-Task-Lease"]};
                socket.close(error); continue; // SYSTEM processed terminal, response was lost.
            }
            http::response<http::string_body> response(http::status::ok,11);
            response.keep_alive(false);
            if (index==0) { response.set(http::field::content_type,"application/json"); response.body()=manifest_body; }
            else if (index<=2) {
                const auto& file=value.manifest.files[static_cast<std::size_t>(index-1)];
                response.set(http::field::content_type,file.content_type);
                response.set("X-Content-SHA256",file.sha256); response.body()=value.bytes.at(file.id);
                if (index==2) {
                    if (mode==1 || mode==2) {
                        response.result(http::status::not_found); response.set(http::field::content_type,"application/json");
                        response.body()=std::string{R"({"ok":false,"code":")"}+
                            (mode==1?"artifact_not_found":"task_not_found")+R"(","message":"synthetic"})";
                    }
                    if (mode==3) {
                        // Declared correct length, dropped stream halfway: uncertainty is transport, not failed resource.
                        const std::string header="HTTP/1.1 200 OK\r\nContent-Type: "+file.content_type+
                            "\r\nContent-Length: "+std::to_string(file.byte_size)+"\r\nX-Content-SHA256: "+file.sha256+"\r\n\r\n";
                        asio::write(socket,asio::buffer(header+response.body().substr(0,2)),error);
                        socket.close(error); continue;
                    }
                    if (mode==4) response.body()[0]='X';
                    if (mode==5) response.set(http::field::content_type,"text/plain");
                    if (mode==6) response.body().push_back('x');
                    if (mode==7) response.set("X-Content-SHA256",std::string(64,'a'));
                }
            } else {
                if (request.body()!=original_request || request["X-Task-Lease"]!=original_lease) server_ok=false;
                response.set(http::field::content_type,"application/json");
                response.body()=R"({"ok":true,"replayed":true,
                  "project_id":"123e4567-e89b-12d3-a456-426614174012","task":{
                  "id":"123e4567-e89b-12d3-a456-426614174010",
                  "batch_id":"123e4567-e89b-12d3-a456-426614174011",
                  "vehicle_id":"123e4567-e89b-12d3-a456-426614174001",
                  "state":"delivered","attempt":2,"max_attempts":3,"progress":100,
                  "error_code":null,"error_message":null,"offered_at":null,
                  "accepted_at":null,"delivered_at":null,"updated_at":"2026-10-04T00:00:00Z"}})";
            }
            response.prepare_payload(); http::write(socket,response,error);
        }
    });
    ClientConfig config; config.base_url="http://127.0.0.1:"+std::to_string(acceptor.local_endpoint().port());
    config.state_directory=root/"state"; config.artifact_directory=root/"packages";
    config.artifact_limits.stream_chunk_bytes=3;
    auto store=DurableStateStore::open(config.state_directory/"state.sqlite3");
    auto client=DeviceTaskHttpClient::create(config,[] { return Result<std::string>{"synthetic-package-token"}; });
    EXPECT(store && client); if (!store || !client) std::abort();
    TaskAvailableMessage notice; notice.task_id=task_id; notice.batch_id=value.manifest.batch_id;
    notice.project_id=value.manifest.project_id; notice.resource_revision_id=value.manifest.resource_revision_id;
    notice.resource_type=ResourceType::map; notice.max_attempts=3;
    EXPECT(store.value()->register_task_generation({task_id,1},0));
    AcceptedTask accepted; accepted.task_id=task_id; accepted.attempt=1; accepted.max_attempts=3;
    accepted.lease_token="synthetic-package-lease"; accepted.lease_expires_at=4102444800;
    EXPECT(store.value()->persist_active_task_lease({notice,{task_id,1},accepted.lease_token,accepted.lease_expires_at,0}));
    auto manifest=client.value()->read_manifest(notice,accepted,*store.value()); EXPECT(manifest);
    if (!manifest) std::abort();
    auto sink=PackageSink::create(config.artifact_directory,manifest.value()); EXPECT(sink); if (!sink) std::abort();
    std::optional<Error> failed;
    for (const auto& file:manifest.value().files) {
        auto download=client.value()->download_artifact_file(manifest.value(),file.id,accepted,*store.value(),
            [&](const std::uint8_t* bytes,std::size_t size) { return sink.value()->write(file.id,bytes,size); });
        if (!download) { failed=download.error(); break; }
        auto sealed=sink.value()->seal_file(file.id,download.value());
        if (!sealed) { failed=sealed.error(); break; }
    }
    if (mode!=0) {
        EXPECT(failed); EXPECT(published_count(config.artifact_directory)==0);
        EXPECT(!store.value()->has_unconfirmed_terminal_event(task_id).value());
        if (failed) {
            const auto classification=classify_resource_failure(*failed,true);
            if (mode==1) EXPECT(classification==ResourceFailure::artifact_file_missing);
            if (mode==2 || mode==3) EXPECT(!classification);
            if (mode==4 || mode==7) EXPECT(classification==ResourceFailure::artifact_hash_mismatch);
            if (mode==5 || mode==6) EXPECT(classification==ResourceFailure::artifact_metadata_mismatch);
        }
    } else {
        EXPECT(!failed); EXPECT(sink.value()->seal()); auto package=sink.value()->commit(); EXPECT(package);
        if (!package) std::abort();
        StatusEventRecord event; event.generation={task_id,1}; event.lease_token=accepted.lease_token;
        event.lease_expires_at=accepted.lease_expires_at; event.progress=20;
        event.local_delivery_evidence=R"({"phase":"download"})";
        for (auto state:{TaskExecutionState::downloading,TaskExecutionState::delivering}) {
            event.state=state; event.event_id=generate_uuid_v4().value(); event.payload_json=make_status_request(event).value();
            EXPECT(store.value()->persist_status_event(event)); EXPECT(store.value()->acknowledge_status_event(event.event_id));
        }
        event.state=TaskExecutionState::delivered; event.progress=100; event.event_id=generate_uuid_v4().value();
        event.local_delivery_evidence=make_map_package_evidence(package.value(),event.generation).value();
        event.payload_json=make_status_request(event).value(); EXPECT(store.value()->persist_status_event(event));
        EXPECT(!client.value()->report_status(event,accepted,*store.value()));
        EXPECT(store.value()->has_unconfirmed_terminal_event(task_id).value());
        store.value().reset(); store=DurableStateStore::open(config.state_directory/"state.sqlite3"); EXPECT(store);
        auto retained=store.value()->status_event(event.event_id); EXPECT(retained);
        EXPECT(retained.value().local_delivery_evidence==event.local_delivery_evidence);
        auto confirmation=client.value()->replay_status_event(retained.value(),*store.value()); EXPECT(confirmation);
        EXPECT(!store.value()->has_unconfirmed_terminal_event(task_id).value());
        EXPECT(store.value()->status_event(event.event_id).value().lease_token.empty());
        EXPECT(verify_map_package(package.value()));
    }
    server.join(); EXPECT(server_ok);
    sink.value().reset(); EXPECT(recover_package_staging(config.artifact_directory).value()==0);
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/"roc-vehicle-package-http";
    std::error_code ec; std::filesystem::remove_all(root,ec);
    for (int mode=0;mode<=7;++mode) flow(root/std::to_string(mode),mode);
    std::filesystem::remove_all(root,ec); return failures ? EXIT_FAILURE:EXIT_SUCCESS;
}
