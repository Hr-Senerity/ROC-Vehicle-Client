#include "../v08_wire_support.hpp"
#include "roc/vehicle/package_receiver.hpp"
#include <csignal>
#include <sys/wait.h>
using namespace v08_wire;
namespace {
[[noreturn]] void kill_self(){::kill(::getpid(),SIGKILL);std::_Exit(99);}
bool killed(pid_t child){int s=0;return ::waitpid(child,&s,0)==child && WIFSIGNALED(s) && WTERMSIG(s)==SIGKILL;}
bool coordination(std::string_view outcome){return outcome=="lease_expired" || outcome=="invalid_state" ||
    outcome=="invalid_transition" || outcome=="invalid_lease" || outcome=="event_conflict";}
std::string publish(const std::filesystem::path& root,const Resource& r){
    if(r.manifest.package_version){auto sink=package_test::prepared(root,r.package);if(!sink)std::_Exit(11);
        auto package=sink.value()->commit();if(!package)std::_Exit(12);
        auto e=make_map_package_evidence(package.value(),{task,1});if(!e)std::_Exit(13);return e.value();}
    auto sink=FileSink::create(root,r.manifest,{"metric"});if(!sink)std::_Exit(14);
    if(!sink.value()->write(reinterpret_cast<const std::uint8_t*>(r.bytes.data()),r.bytes.size()) ||
        !sink.value()->seal(road_test::metadata(r.manifest)))std::_Exit(15);
    auto result=sink.value()->commit();if(!result)std::_Exit(16);
    Json e(jsoncons::json_object_arg);e["receiver"]="verified_file";e["task_id"]=task;e["attempt"]=1;
    e["schema_version"]=2;e["resource_revision_id"]=r.manifest.resource_revision_id;e["sha256"]=r.manifest.sha256;
    e["byte_size"]=r.manifest.byte_size;e["name"]=result.value().path.filename().string();return e.to_string();
}
void flow(const std::filesystem::path& root,int kind,const std::string& outcome,int window,
          const Certificate* certificate_value=nullptr,const std::filesystem::path& ca={}){
    const auto r=resource(kind);const bool reconcile=coordination(outcome);
    const bool uncertain=outcome!="success" && !reconcile;
    // A valid historical replay returns current attempt 2. A subsequent new
    // offered accept must therefore advance to 3, never reset to 2. Rejected
    // old attempt-1 events expose no newer snapshot, so reoffer advances to 2.
    const int next_attempt=reconcile?2:3;
    Server server;auto c=config(root,server.port(),certificate_value!=nullptr);
    if(certificate_value)c.tls.ca_file=ca;
    const auto db=c.state_directory/"state.sqlite3";
    ssl::context tls_context(ssl::context::tls_server);
    if(certificate_value){tls_context.use_certificate_chain(asio::buffer(certificate_value->cert));
        tls_context.use_private_key(asio::buffer(certificate_value->key),ssl::context::pem);}
    const std::int64_t expiry=window==0 && outcome=="success"?4102444800:1;
    StatusEventRecord original;original.event_id=event_id;original.generation={task,1};
    original.state=TaskExecutionState::delivered;original.progress=100;original.lease_token=lease;original.lease_expires_at=expiry;
    original.payload_json=make_status_request(original).value();
    const auto expected_body=original.payload_json;
    bool platform_saved=false;int status_index=0;
    std::atomic<bool> injection_finished{false};
    // A child is killed after durable publication + Outbox, optionally after a
    // real failed HTTP exchange. No server thread exists at fork time.
    const pid_t child=::fork();CHECK(child>=0);if(child<0)return;
    if(child==0){
        auto store=DurableStateStore::open(db);if(!store)std::_Exit(10);
        const auto a=seed(*store.value(),r.manifest,true,expiry);if(failures)std::_Exit(17);
        auto event=original;event.local_delivery_evidence=publish(c.artifact_directory,r);
        if(!store.value()->persist_status_event(event))std::_Exit(18);
        if(window>0){auto http_client=client(c);if(!http_client)std::_Exit(19);
            auto result=http_client.value()->report_status(event,a,*store.value());if(result)std::_Exit(20);}
        kill_self();
    }
    const int count=(window>0?1:0)+1+(uncertain?1:0)+3;
    const auto exchange=[&](auto& socket){
        const bool accept=server.targets.size()==static_cast<std::size_t>((window>0?1:0)+1+(uncertain?1:0));
        if(accept){
            if(!request(socket,server,task_accept_target(task),http::verb::post))return;
            if(!server.leases.back().empty() || !server.bodies.back().empty())server.good=false;
            Json j(jsoncons::json_object_arg);j["ok"]=true;j["replayed"]=false;
            j["lease_token"]="synthetic-v08-new-lease";j["lease_expires_at"]=4102444800LL;
            j["task"]=snapshot("accepted",next_attempt,reconcile?75:100);json(socket,j.to_string());return;
        }
        if(!request(socket,server,task_status_target(task),http::verb::post))return;
        const auto sent=Json::parse(server.bodies.back());
        if(sent["event_id"].as<std::string>()!=event_id){
            if(server.leases.back()!="synthetic-v08-new-lease" || sent["progress"].as<int>()!=(reconcile?80:100))server.good=false;
            Json j=Json::parse(status_response(false));j["task"]=snapshot(sent["state"].as<std::string>(),next_attempt,reconcile?80:100);
            json(socket,j.to_string());return;
        }
        if(server.bodies.back()!=expected_body || server.leases.back()!=lease)server.good=false;
        const int index=status_index++;
        if(window>0 && index==0){
            // Known processed/lost response versus unknown effect. In window 2
            // the fixture saves only when not testing an explicit rejection.
            platform_saved=window==1 || !reconcile;
            boost::system::error_code ec;beast::get_lowest_layer(socket).close(ec);return;
        }
        const int recovery_index=index-(window>0?1:0);
        if(outcome=="success" || (uncertain && recovery_index>0)){
            if(expiry==1 && !platform_saved){server.good=false;return;}
            const bool replayed=platform_saved;platform_saved=true;json(socket,status_response(replayed));return;
        }
        if(reconcile){json(socket,"{\"ok\":false,\"code\":\""+outcome+
            "\",\"message\":\"synthetic delivered success -- do not interpret prose\"}",outcome=="invalid_lease"?401U:409U);return;}
        if(outcome=="unknown"){json(socket,R"({"ok":false,"code":"future_code","message":"delivered"})",409);return;}
        if(outcome=="malformed"){json(socket,"{broken");return;}
        if(outcome=="truncated"){
            boost::system::error_code ec;asio::write(socket,asio::buffer(std::string{
                "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 1000\r\n\r\n{\"ok\":"}),ec);return;}
        if(outcome=="timeout"){
            std::this_thread::sleep_for(std::chrono::milliseconds{450});injection_finished.store(true);return;}
        if(outcome=="500"){json(socket,R"({"ok":false,"code":"internal_error","message":"delivered"})",500);return;}
        if(outcome=="authentication_failed"){json(socket,R"({"ok":false,"code":"authentication_failed","message":"delivered"})",401);return;}
        if(outcome=="wrong_task"){auto j=Json::parse(status_response());j["task"]["id"]="123e4567-e89b-12d3-a456-426614174099";json(socket,j.to_string());return;}
        if(outcome=="missing_field"){auto j=Json::parse(status_response());j.erase("project_id");json(socket,j.to_string());return;}
        if(outcome=="wrong_mime"){
            boost::system::error_code ec;const auto body=status_response();asio::write(socket,asio::buffer(
                "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\nContent-Length: "+std::to_string(body.size())+"\r\n\r\n"+body),ec);return;}
        server.good=false;
    };
    server.start([&](tcp::socket& socket){
        if(certificate_value){ssl::stream<tcp::socket&> stream(socket,tls_context);
            boost::system::error_code ec;stream.handshake(ssl::stream_base::server,ec);
            if(ec){server.good=false;return;}exchange(stream);stream.shutdown(ec);
        }else exchange(socket);
    },count);
    CHECK(killed(child));
    auto store=DurableStateStore::open(db);CHECK(store);if(!store){server.finish();return;}
    original=store.value()->status_event(event_id).value();
    const auto evidence=original.local_delivery_evidence;
    CHECK(!evidence.empty());CHECK(original.request_sha256==sha256_hex(original.payload_json).value());
    CHECK(original.lease_expires_at==expiry);CHECK(original.payload_json==make_status_request(original).value());
    if(r.manifest.package_version)CHECK(inspect_map_package(c.artifact_directory,r.manifest));
    else CHECK(inspect_file_artifact(c.artifact_directory,r.manifest,{"metric"}));
    auto http_client=client(c);CHECK(http_client);if(!http_client){server.finish();return;}
    CHECK(!http_client.value()->accept_task(notice(r.manifest),*store.value()));
    CHECK(!http_client.value()->recover_accepted_task({task,1},*store.value()));
    CHECK(!store.value()->task_generation({task,2}));CHECK(!store.value()->mark_task_generation_expired({task,1}));
    auto changed=original;changed.error_message="replacement";changed.payload_json=make_status_request(changed).value();
    CHECK(!http_client.value()->replay_status_event(changed,*store.value()));
    const auto reply=http_client.value()->replay_status_event(original,*store.value());
    if(outcome=="success"){CHECK(reply);if(reply){CHECK(reply.value().attempt==2);CHECK(reply.value().replayed==(window>0));}}
    else{
        CHECK(!reply);const auto retained=store.value()->status_event(event_id).value();
        CHECK(retained.generation.task_id==original.generation.task_id && retained.generation.attempt==1);
        CHECK(retained.event_id==original.event_id && retained.payload_json==original.payload_json && retained.request_sha256==original.request_sha256);
        CHECK(retained.lease_token==original.lease_token && retained.lease_expires_at==expiry && retained.local_delivery_evidence==evidence);
        CHECK(retained.disposition==(reconcile?StatusEventDisposition::reconciliation_required:StatusEventDisposition::pending));
        if(reconcile){CHECK(!reply && reply.error().server_code==outcome);CHECK(retained.reconciliation_reason==outcome);}
        if(outcome=="timeout")CHECK(!reply && reply.error().code==ErrorCode::timeout);
        CHECK(!http_client.value()->accept_task(notice(r.manifest),*store.value()));
        auto replacement=original;replacement.event_id=generate_uuid_v4().value();replacement.state=TaskExecutionState::failed;
        replacement.error_code="local_storage_failure";replacement.payload_json=make_status_request(replacement).value();
        CHECK(!store.value()->persist_status_event(replacement));
        if(reconcile){
            store.value().reset();const pid_t resolver=::fork();CHECK(resolver>=0);
            // Only async-signal-safe exec is used in the multithreaded fork
            // child; all SQLite/C++ work runs in a fresh process.
            if(resolver==0){::execl("/proc/self/exe","roc-v08-resolver","--resolve",db.c_str(),static_cast<char*>(nullptr));std::_Exit(21);}
            CHECK(killed(resolver));store=DurableStateStore::open(db);CHECK(store);
            CHECK(store.value()->status_event(event_id).value().disposition==StatusEventDisposition::resolved);
        }else{
            if(outcome=="timeout"){
                // Controlled application retry after the injected stalled
                // exchange has drained; no automatic client retry is assumed.
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds{2};
                while(!injection_finished.load() && std::chrono::steady_clock::now()<deadline)
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                CHECK(injection_finished.load());
            }
            store.value().reset();store=DurableStateStore::open(db);CHECK(store);
            auto retry=http_client.value()->replay_status_event(store.value()->status_event(event_id).value(),*store.value());
            CHECK(retry);if(retry)CHECK(retry.value().replayed);
        }
    }
    CHECK(store.value()->status_event(event_id).value().lease_token.empty());
    CHECK(store.value()->status_event(event_id).value().local_delivery_evidence==evidence);
    CHECK(!store.value()->has_unconfirmed_terminal_event(task).value());CHECK(store.value()->active_task_keys().value().empty());
    auto reoffered_notice=notice(r.manifest);reoffered_notice.attempt=next_attempt-1;
    const auto next=http_client.value()->accept_task(reoffered_notice,*store.value());CHECK(next);
    if(next){
        CHECK(next.value().attempt==next_attempt && next.value().progress_floor==(reconcile?75:100));
        CHECK(next.value().lease_token!=lease);CHECK(store.value()->active_task_lease({task,next_attempt}));
        AcceptedTask old;old.task_id=task;old.attempt=1;old.lease_token=lease;old.lease_expires_at=4102444800;
        CHECK(!http_client.value()->read_manifest(notice(r.manifest),old,*store.value()));
        auto forged=next.value();forged.lease_token=lease;CHECK(!http_client.value()->read_manifest(notice(r.manifest),forged,*store.value()));
        const auto rejected_download=[&](const AcceptedTask& handle){
            const auto never=[](const std::uint8_t*,std::size_t)->Result<void>{CHECK(false);return {};};
            return r.manifest.package_version?
                http_client.value()->download_artifact_file(r.manifest,r.manifest.files.front().id,handle,*store.value(),never):
                http_client.value()->download_artifact(r.manifest,handle,*store.value(),never);
        };
        CHECK(!rejected_download(old));CHECK(!rejected_download(forged));
        for(const auto state:{TaskExecutionState::downloading,TaskExecutionState::delivering}){
            StatusEventRecord e;e.event_id=generate_uuid_v4().value();e.generation={task,next_attempt};e.state=state;
            e.progress=next.value().progress_floor-1;e.lease_token=next.value().lease_token;e.lease_expires_at=next.value().lease_expires_at;
            e.local_delivery_evidence=R"({"phase":"new_attempt"})";e.payload_json=make_status_request(e).value();
            CHECK(!store.value()->persist_status_event(e));
            if(reconcile && state==TaskExecutionState::delivering){
                e.progress=79;e.payload_json=make_status_request(e).value();CHECK(!store.value()->persist_status_event(e));}
            e.progress=reconcile?80:100;e.payload_json=make_status_request(e).value();
            e.lease_token=lease;CHECK(!store.value()->persist_status_event(e));e.lease_token=next.value().lease_token;
            CHECK(store.value()->persist_status_event(e));CHECK(http_client.value()->report_status(e,next.value(),*store.value()));
            CHECK(store.value()->active_task_lease({task,next_attempt}));
            store.value().reset();store=DurableStateStore::open(db);CHECK(store);
            auto active=http_client.value()->recover_accepted_task({task,next_attempt},*store.value());CHECK(active);
            if(active)CHECK(active.value().lease_token==next.value().lease_token && active.value().progress_floor==e.progress);
            CHECK(!http_client.value()->accept_task(notice(r.manifest),*store.value()));
        }
    }
    server.finish();CHECK(server.good);CHECK(server.connections==count);
    const int exact_count=(window>0?1:0)+1+(uncertain?1:0);
    CHECK(server.targets.size()==static_cast<std::size_t>(count));
    for(int i=0;i<exact_count && static_cast<std::size_t>(i)<server.bodies.size();++i){
        CHECK(server.bodies[static_cast<std::size_t>(i)]==original.payload_json);
        CHECK(server.leases[static_cast<std::size_t>(i)]==lease);CHECK(server.targets[static_cast<std::size_t>(i)]==task_status_target(task));}
    CHECK(store.value()->pending_status_events().value().empty());
    CHECK(!store.value()->task_generation({task,next_attempt+1}));
    std::cout<<"G2-G4 "<<r.name<<(certificate_value?" HTTPS":" HTTP")<<" SIGKILL window="<<window<<" outcome="<<outcome
        <<" expired="<<(expiry==1)<<" exact_status="<<exact_count<<" accept_after_resolution=1 attempt="<<next_attempt<<'\n';
}
}
int main(int argc,char** argv){
    if(argc==3 && std::string{argv[1]}=="--resolve"){
        auto store=DurableStateStore::open(argv[2]);
        if(!store || !store.value()->resolve_status_event_reconciliation(event_id))return 21;
        kill_self();
    }
    const auto root=std::filesystem::temp_directory_path()/"roc-v08-wire-recovery";
    std::error_code ec;std::filesystem::remove_all(root,ec);
    std::filesystem::create_directories(root);
    CHECK(::chmod(root.c_str(),0700)==0);const auto cert=certificate();const auto ca=root/"synthetic-ca.pem";save_ca(ca,cert);
    for(const int kind:{4,6}){
        flow(root/(std::to_string(kind)+"-unsent"),kind,"success",0);
        flow(root/(std::to_string(kind)+"-processed-lost"),kind,"success",1);
        flow(root/(std::to_string(kind)+"-unknown-processed"),kind,"success",2);
        for(const std::string code:{"lease_expired","invalid_state","invalid_transition","invalid_lease","event_conflict"})
            flow(root/(std::to_string(kind)+code),kind,code,2);
        for(const std::string outcome:{"unknown","malformed","truncated","timeout","500","wrong_task","missing_field","wrong_mime","authentication_failed"})
            flow(root/(std::to_string(kind)+outcome),kind,outcome,2);
        flow(root/(std::to_string(kind)+"-tls-unsent"),kind,"success",0,&cert,ca);
        flow(root/(std::to_string(kind)+"-tls-lost"),kind,"success",1,&cert,ca);
        flow(root/(std::to_string(kind)+"-tls-expired"),kind,"lease_expired",2,&cert,ca);
        flow(root/(std::to_string(kind)+"-tls-timeout"),kind,"timeout",2,&cert,ca);
    }
    std::filesystem::remove_all(root,ec);return failures?EXIT_FAILURE:EXIT_SUCCESS;}
