#include "../driver_joint_support.hpp"
#include <functional>
#include <sys/stat.h>

namespace {
using namespace driver_test;
int scenarios=0;
void configure(const std::filesystem::path& root,const Json& config) {
    write_private(root/"device-token",driver_token);write_private(root/"config.json",config.to_string());
}
bool completed(JointFixture& server,int count=1) {
    return server.inspect([&](const auto& f) {int total=0;for(const auto& t:f.tasks)total+=t.delivered;return total>=count;});
}
Json owning_context(const ArtifactManifest& m,const char* mode="metric") {
    Json row(jsoncons::json_object_arg);row["project_id"]=m.project_id;row["resource_revision_id"]=m.resource_revision_id;
    row["artifact_sha256"]=m.sha256;row["map_id"]=m.map_id;row["map_revision_id"]="123e4567-e89b-12d3-a456-426614174099";
    row["coordinate_mode"]=mode;
    Json j(jsoncons::json_object_arg);j["context_version"]=1;j["system_commit"]="a9c601d6977d89000438854a24e3199573004a2f";
    j["vehicle_id"]=vehicle_id;j["bindings"]=Json(jsoncons::json_array_arg);j["bindings"].push_back(row);return j;
}
void context_prechecks() {
    const auto root=temporary_root();{
        JointFixture server(root,6);auto cfg=settings(root,server.port());
        cfg["base_url"]="https://vehicle.invalid";cfg["transport"]="tls";cfg["network_scope"]="remote";
        configure(root,cfg);
        Process missing(root,root/"config.json",{"--precheck","--allow-remote"});CHECK(missing.wait()==2);missing.redacted();
        cfg["owning_map_context_file"]=(root/"context.json").string();write_private(root/"config.json",cfg.to_string());
        const auto original=owning_context(server.tasks[0].input.manifest);int cases=0;
        const auto check=[&](const Json& j,int expected) {
            write_private(root/"context.json",j.to_string());
            Process p(root,root/"config.json",{"--precheck","--allow-remote"});CHECK(p.wait()==expected);p.redacted();++cases;
        };
        check(original,0);
        for(const auto* field:{"context_version","system_commit","vehicle_id","bindings"}) {auto j=original;j.erase(field);check(j,2);}
        auto j=original;j["system_commit"]="wrong";check(j,2);j=original;j["vehicle_id"]="123e4567-e89b-12d3-a456-426614174098";check(j,2);
        j=original;j["context_version"]=2;check(j,2);j=original;j["unexpected"]=true;check(j,2);
        j=original;j["bindings"]=Json(jsoncons::json_array_arg);check(j,2);
        j=original;j["bindings"].push_back(j["bindings"][0]);check(j,2);
        for(const auto* field:{"project_id","resource_revision_id","map_id","map_revision_id","artifact_sha256","coordinate_mode"}) {
            j=original;j["bindings"][0][field]="bad";check(j,2);
        }
        j=original;j["bindings"][0]["extra"]=1;check(j,2);
        write_private(root/"context.json",original.to_string());CHECK(::chmod((root/"context.json").c_str(),0644)==0);
        Process wide(root,root/"config.json",{"--precheck","--allow-remote"});CHECK(wide.wait()==2);wide.redacted();
        CHECK(::chmod((root/"context.json").c_str(),0600)==0);
        CHECK(server.inspect([](const auto& s){return s.handshakes==0 && s.requests.empty();}));
        CHECK(!std::filesystem::exists(root/"state"));CHECK(!std::filesystem::exists(root/"artifacts"));
        std::cout<<"S1 protected_context_prechecks cases="<<cases+2<<" remote_missing_refused=1 sockets=0\n";
    }std::filesystem::remove_all(root);++scenarios;
}
void context_delivery(int kind,const char* artifact_mode,int mismatch) {
    const auto root=temporary_root();{
        JointFixture server(root,kind);
        server.inspect([&](auto& s) {
            auto& r=s.tasks[0].input;auto j=Json::parse(r.bytes);j["coordinate_mode"]=artifact_mode;
            // Scale only this synthetic fixture; the client must keep these original samples.
            if(kind==6 && std::string(artifact_mode)=="legacy-normalized") {
                for(auto& n:j["nodes"].array_range()) {n["x"]=n["x"].template as<double>()/4;n["y"]=n["y"].template as<double>()/4;}
                for(auto& e:j["edges"].array_range())for(const auto* c:{"control1","control2"}) {
                    e["geometry"][c]["x"]=e["geometry"][c]["x"].template as<double>()/4;
                    e["geometry"][c]["y"]=e["geometry"][c]["y"].template as<double>()/4;
                }
                for(auto& t:j["trajectories"].array_range()) {t["length"]=t["length"].template as<double>()/4;
                    for(auto& p:t["points"].array_range())for(const auto* c:{"x","y","s"})p[c]=p[c].template as<double>()/4;}
            }
            r.bytes=j.to_string();r.manifest.sha256=sha256_hex(r.bytes).value();r.manifest.byte_size=r.bytes.size();
        });
        const auto& m=server.tasks[0].input.manifest;auto context=owning_context(m,artifact_mode);
        if(mismatch>=1 && mismatch<=4) {
            const std::array<const char*,4> fields{"resource_revision_id","project_id","map_id","artifact_sha256"};
            context["bindings"][0][fields[static_cast<std::size_t>(mismatch-1)]]=mismatch==4?std::string(64,'0'):"123e4567-e89b-12d3-a456-426614174098";
        } else if(mismatch==5)context["bindings"][0]["coordinate_mode"]=std::string(artifact_mode)=="metric"?"legacy-normalized":"metric";
        auto cfg=settings(root,server.port());cfg["owning_map_context_file"]=(root/"context.json").string();
        write_private(root/"context.json",context.to_string());configure(root,cfg);
        Process driver(root,root/"config.json");
        if(!mismatch) {
            CHECK(eventually([&]{return completed(server);}));CHECK(driver.contains("owning_map_context_bound"));
            driver.signal(SIGTERM);CHECK(driver.wait()==0);driver.redacted();
            const auto final=inspect_file_artifact(root/"artifacts"/("task-"+m.task_id+"-1"),m,{artifact_mode,64U*1024U*1024U});
            CHECK(final);if(final)CHECK(road_test::read(final.value().path)==server.tasks[0].input.bytes);
            auto db=DurableStateStore::open(root/"state"/"roc-vehicle-state.sqlite3");CHECK(db);
            server.inspect([&](const auto& s){for(const auto& e:s.tasks[0].events) {
                const auto saved=db.value()->status_event(e.first);CHECK(saved);
                if(saved && saved.value().state==TaskExecutionState::delivered)
                    CHECK(Json::parse(saved.value().local_delivery_evidence).contains("owning_map_context_sha256"));
            }});
        } else {
            CHECK(eventually([&]{return server.inspect([](const auto& s){return s.tasks[0].failed==1;});}));
            driver.signal(SIGTERM);CHECK(driver.wait()==0);driver.redacted();
            CHECK(server.inspect([&](const auto& s){return s.good && s.tasks[0].delivered==0 &&
                s.tasks[0].downloads==(mismatch==5?1:0) && s.tasks[0].failure_code=="artifact_metadata_mismatch";}));
            CHECK(!inspect_file_artifact(root/"artifacts"/("task-"+m.task_id+"-1"),m));
        }
        std::cout<<"S1 trusted_context schema="<<kind-4<<" artifact_mode="<<artifact_mode<<" mismatch="<<mismatch
                 <<" original_bytes_preserved_or_delivered_refused=1\n";
    }std::filesystem::remove_all(root);++scenarios;
}
void normal(int kind) {
    const auto root=temporary_root();
    {
        JointFixture server(root,kind);configure(root,settings(root,server.port()));
        Process driver(root,root/"config.json");CHECK(eventually([&] {return completed(server);}));
        CHECK(eventually([&] {return server.inspect([](const auto& s) {return s.heartbeats>=1;});}));
        server.notify(0,32);CHECK(eventually([&] {return driver.contains("duplicate_completed");}));
        CHECK(server.inspect([&](const auto& s) {return s.good && s.tasks[0].accepts==1 && s.tasks[0].delivered==1 &&
            s.tasks[0].downloads==(kind==4?2:1) && s.tasks[0].manifests==1;}));
        driver.signal(SIGTERM);CHECK(driver.wait()==0);driver.redacted();
        const auto& m=server.tasks[0].input.manifest;
        const auto artifacts=root/"artifacts"/("task-"+m.task_id+"-1");
        if(m.package_version==2) {
            const auto found=inspect_map_package(artifacts,m);CHECK(found);
            if(found)for(const auto& file:m.files)CHECK(package_test::read(found.value().directory/(file.role+".bin"))==server.tasks[0].input.package.bytes.at(file.id));
        } else {
            const auto found=inspect_file_artifact(artifacts,m);CHECK(found);
            if(found)CHECK(road_test::read(found.value().path)==server.tasks[0].input.bytes);
        }
        if(kind==4 || kind==6) {
            server.reoffer(0); // Same id, new legitimate offered attempt, progress remains 100.
            Process next(root,root/"config.json");CHECK(eventually([&] {return completed(server,2);}));
            CHECK(server.inspect([](const auto& s) {return s.good && s.tasks[0].attempt==2 && s.tasks[0].accepts==2 && s.tasks[0].progress==100;}));
            // A higher offered attempt must also reach the SAME running worker
            // after the old notification slot was released, without reconnect.
            const auto handshakes=server.inspect([](const auto& s){return s.handshakes;});
            server.reoffer(0);server.notify(0,2);CHECK(eventually([&]{return completed(server,3);}));
            CHECK(server.inspect([&](const auto& s){return s.good && s.handshakes==handshakes &&
                s.tasks[0].attempt==3 && s.tasks[0].accepts==3 && s.tasks[0].progress==100;}));
            next.signal(SIGTERM);CHECK(next.wait()==0);next.redacted();
        }
        std::cout<<"H2 normal resource="<<resource(kind).name<<" actual_WS_HTTP durable=1 duplicate_notifications=32 restart_and_same_process_reoffer="<<(kind==4 || kind==6)<<'\n';
    }
    std::filesystem::remove_all(root);++scenarios;
}
void context_publication_recovery() {
    const auto root=temporary_root();{
        JointFixture server(root,6);const auto& m=server.tasks[0].input.manifest;
        const auto context=owning_context(m);write_private(root/"context.json",context.to_string());
        auto cfg=settings(root,server.port());cfg["owning_map_context_file"]=(root/"context.json").string();configure(root,cfg);
        Process probe(root,root/"config.json",{"--run"},ROC_DRIVER_CRASH_PROBE);
        CHECK(probe.wait()==128+SIGKILL);CHECK(probe.contains("publication_committed"));probe.redacted();
        const auto downloads=server.inspect([](const auto& s){return s.tasks[0].downloads;});
        for(const auto* field:{"coordinate_mode","resource_revision_id","project_id","map_id","map_revision_id","artifact_sha256"}) {
            auto changed=context;changed["bindings"][0][field]=std::string(field)=="coordinate_mode"?"legacy-normalized":
                std::string(field)=="artifact_sha256"?std::string(64,'0'):"123e4567-e89b-12d3-a456-426614174098";
            write_private(root/"context.json",changed.to_string());
            Process conflict(root,root/"config.json");CHECK(conflict.wait()==5);conflict.redacted();
            CHECK(server.inspect([&](const auto& s){return s.good && s.tasks[0].accepts==1 && s.tasks[0].delivered==0 &&
                s.tasks[0].failed==0 && s.tasks[0].downloads==downloads;}));
        }
        CHECK(inspect_file_artifact(root/"artifacts"/("task-"+m.task_id+"-1"),m));
        write_private(root/"context.json",context.to_string());
        Process recovered(root,root/"config.json");CHECK(eventually([&]{return completed(server);}));
        CHECK(recovered.contains("publication_revalidated"));recovered.signal(SIGTERM);CHECK(recovered.wait()==0);recovered.redacted();
        CHECK(server.inspect([&](const auto& s){return s.good && s.tasks[0].accepts==1 && s.tasks[0].downloads==downloads;}));
        std::cout<<"S1 context_publication_recovery changed_trust_refused=6 no_new_failed=1 original_final_retained=1 original_context_recovery=1\n";
    }std::filesystem::remove_all(root);++scenarios;
}
void reconnect_timeout_and_restart() {
    const auto root=temporary_root();
    {
        JointFixture server(root,6);server.inspect([](auto& s) {s.stall_download=true;});configure(root,settings(root,server.port()));
        Process driver(root,root/"config.json");CHECK(eventually([&] {return completed(server);}));
        CHECK(driver.contains("download_retry"));CHECK(eventually([&] {return server.inspect([](const auto& s) {return s.heartbeats>=1;});}));
        server.disconnect();CHECK(eventually([&] {return server.inspect([](const auto& s) {return s.handshakes>=2 && s.heartbeats>=2;});}));
        server.notify(0,4);CHECK(eventually([&] {return driver.contains("duplicate_completed");}));
        driver.signal(SIGTERM);CHECK(driver.wait()==0);driver.redacted();
        const auto count=server.inspect([](const auto& s) {return s.requests.size();});
        Process again(root,root/"config.json");CHECK(eventually([&] {return again.contains("startup_recovery_complete");}));
        server.notify(0,4);CHECK(eventually([&] {return again.contains("duplicate_completed");}));
        again.signal(SIGTERM);CHECK(again.wait()==0);again.redacted();
        CHECK(server.inspect([&](const auto& s) {return s.good && s.requests.size()==count && s.tasks[0].accepts==1 && s.tasks[0].delivered==1 && s.tasks[0].downloads==2;}));
        std::cout<<"H4 timeout_download_exact_restart WS_reconnect heartbeat_cursor duplicate_after_SIGTERM no_new_accept=1\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void security_stops() {
    for(int mode=0;mode<3;++mode) {
        const auto root=temporary_root();{
            JointFixture server(root,0);server.inspect([&](auto& s) {s.notify_enabled=false;s.wrong_identity=mode==0;s.ws_error=mode==1?"credential_revoked":mode==2?"connection_replaced":"";});
            configure(root,settings(root,server.port()));Process driver(root,root/"config.json");CHECK(driver.wait()==(mode==2?7:4));driver.redacted();
            CHECK(server.inspect([](const auto& s) {return s.good && s.handshakes==1 && s.requests.empty();}));
            CHECK(driver.contains(mode==2?"connection_replaced":"authentication_stopped"));
            std::cout<<"H4 stop_identity_auth_replaced mode="<<mode<<" HTTP_requests=0 blind_reconnect=0\n";
        }
        std::filesystem::remove_all(root);++scenarios;
    }
}
void known_hash_failure() {
    const auto root=temporary_root();{
        JointFixture server(root,4);server.inspect([](auto& s) {s.hash_failure=true;});configure(root,settings(root,server.port()));
        Process driver(root,root/"config.json");CHECK(eventually([&] {return server.inspect([](const auto& s) {return s.tasks[0].failed==1;});}));
        driver.signal(SIGTERM);CHECK(driver.wait()==0);driver.redacted();
        CHECK(server.inspect([](const auto& s) {return s.good && s.tasks[0].delivered==0 && s.tasks[0].accepts==1;}));
        auto db=DurableStateStore::open(root/"state"/"roc-vehicle-state.sqlite3");CHECK(db);
        if(db) {CHECK(db.value()->pending_status_events().value().empty());CHECK(db.value()->active_task_keys().value().empty());}
        std::cout<<"H2 known_hash_failure stable_failed=1 delivered=0\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void offline_budget() {
    const auto root=temporary_root();{
        JointFixture server(root,0);server.inspect([](auto& s){s.ignore_hello=true;});
        auto cfg=settings(root,server.port());cfg["hello_timeout_ms"]=50;cfg["offline_budget_ms"]=200;configure(root,cfg);
        Process driver(root,root/"config.json");CHECK(driver.wait()==9);driver.redacted();CHECK(driver.contains("offline_budget_exhausted"));
        CHECK(server.inspect([](const auto& s){return s.good && s.handshakes>=1 && s.requests.empty();}));
        std::cout<<"H4 bounded_offline_budget no_hello no_HTTP no_infinite_initial_retry=1\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void prechecks() {
    const auto root=temporary_root();{
        JointFixture server(root,0);const auto original=settings(root,server.port());configure(root,original);
        int cases=0;
        const auto check=[&](Json j,int expected,std::vector<std::string> args={"--precheck"}) {
            write_private(root/"config.json",j.to_string());Process p(root,root/"config.json",std::move(args));
            CHECK(p.wait()==expected);p.redacted();++cases;
        };
        check(original,0);CHECK(!std::filesystem::exists(root/"state"));CHECK(!std::filesystem::exists(root/"artifacts"));
        for(const auto* field:{"base_url","vehicle_id","token_file","state_directory","artifact_directory","transport"}) {
            auto j=original;j.erase(field);check(j,2);
        }
        for(const auto* bad:{"","bad-uuid"}) {auto j=original;j["vehicle_id"]=bad;check(j,2);}
        for(const auto* bad:{"/","/tmp","/var/tmp","/build","/workspace","/dev/shm"}) {
            auto j=original;j["state_directory"]=bad;check(j,2);
        }
        struct stat ancestor{};CHECK(::stat("/tmp",&ancestor)==0);const auto ancestor_mode=ancestor.st_mode;
        const auto wide=root/"wide";CHECK(::mkdir(wide.c_str(),0755)==0);auto j=original;j["artifact_directory"]=wide.string();check(j,2);
        struct stat info{};CHECK(::stat(wide.c_str(),&info)==0 && (info.st_mode&0777)==0755);
        CHECK(::stat("/tmp",&info)==0 && info.st_mode==ancestor_mode);CHECK(!std::filesystem::exists(root/"state"/".roc-driver.lock"));
        j=original;j["artifact_directory"]=(root/"state"/"child").string();check(j,2);
        j=original;j["state_directory"]="relative";check(j,2);
        j=original;j["state_directory"]=(root/"missing"/"child").string();check(j,2);
        CHECK(::symlink(wide.c_str(),(root/"linked").c_str())==0);j=original;j["state_directory"]=(root/"linked"/"child").string();check(j,2);
        j=original;j["token"]=driver_token;check(j,2);
        check(original,2,{"--precheck","--token",driver_token});
        for(const auto* text:{"","bad\nembedded"}) {write_private(root/"device-token",text);check(original,2);}
        write_private(root/"device-token",driver_token);CHECK(::chmod((root/"device-token").c_str(),0644)==0);check(original,2);CHECK(::chmod((root/"device-token").c_str(),0600)==0);
        CHECK(::symlink((root/"device-token").c_str(),(root/"link-token").c_str())==0);j=original;j["token_file"]=(root/"link-token").string();check(j,2);
        CHECK(::link((root/"device-token").c_str(),(root/"hard-token").c_str())==0);check(original,2);CHECK(::unlink((root/"hard-token").c_str())==0);
        CHECK(::mkfifo((root/"fifo-token").c_str(),0600)==0);j=original;j["token_file"]=(root/"fifo-token").string();check(j,2);
        j=original;j["queue_capacity"]=0;check(j,2);j=original;j["retry_attempts"]=11;check(j,2);
        j=original;j["base_url"]="https://vehicle.invalid";j["transport"]="tls";check(j,2);
        j["network_scope"]="remote";check(j,2);check(j,2,{"--precheck","--allow-remote"});
        j=original;j["base_url"]="http://localhost:1234";check(j,2);
        j=original;j["transport"]="tls";check(j,2);j=original;j["insecure_demo"]=true;check(j,2);
        write_private(root/"bad-ca.pem","not a CA");j=original;j["ca_file"]=(root/"bad-ca.pem").string();check(j,2);
        check(original,2,{"--resolve",event_id}); // Human confirmation is mandatory.
        check(original,0,{"--help"});
        CHECK(server.inspect([](const auto& s) {return s.requests.empty() && s.handshakes==0;}));
        CHECK(!std::filesystem::exists(root/"state"));CHECK(!std::filesystem::exists(root/"artifacts"));
        std::cout<<"H1 executable_prechecks cases="<<cases<<" sockets=0 shared_ancestor_unchanged=1 no_created_roots=1 secrets_redacted=1\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void exclusive_stop() {
    const auto root=temporary_root();{
        JointFixture server(root,6);server.inspect([](auto& s) {s.stall_download=true;});configure(root,settings(root,server.port()));
        Process first(root,root/"config.json");CHECK(eventually([&] {return server.inspect([](const auto& s) {return s.download_stalled;});}));
        Process same(root,root/"config.json");CHECK(same.wait()==3);CHECK(same.contains("root_in_use"));same.redacted();
        auto other=settings(root,server.port());other["state_directory"]=(root/"other-state").string();write_private(root/"other.json",other.to_string());
        Process same_artifacts(root,root/"other.json");CHECK(same_artifacts.wait()==3);CHECK(same_artifacts.contains("root_in_use"));same_artifacts.redacted();
        other=settings(root,server.port());other["artifact_directory"]=(root/"other-artifacts").string();write_private(root/"other.json",other.to_string());
        Process same_state(root,root/"other.json");CHECK(same_state.wait()==3);CHECK(same_state.contains("root_in_use"));same_state.redacted();
        CHECK(server.inspect([](const auto& s) {return s.handshakes==1 && s.tasks[0].accepts==1;}));
        first.signal(SIGTERM);CHECK(first.wait()==0);first.redacted();
        server.release_stalls();Process restarted(root,root/"config.json");CHECK(eventually([&] {return completed(server);}));
        CHECK(restarted.contains("active_recovery_no_accept"));restarted.signal(SIGTERM);CHECK(restarted.wait()==0);restarted.redacted();
        CHECK(server.inspect([](const auto& s) {return s.good && s.tasks[0].accepts==1;}));
        struct stat info{};CHECK(::stat((root/"state"/".roc-driver.lock").c_str(),&info)==0 && (info.st_mode&0777)==0600);
        std::cout<<"H4 exclusive_state_and_artifact_roots competitors=3 SIGTERM_original_lease_restart=1\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void pressure() {
    const auto root=temporary_root();{
        JointFixture server(root,6,3);server.inspect([](auto& s) {s.notify_enabled=false;s.stall_download=true;});
        auto cfg=settings(root,server.port());cfg["queue_capacity"]=1;configure(root,cfg);
        Process driver(root,root/"config.json");CHECK(eventually([&] {return driver.contains("startup_recovery_complete");}));
        server.notify(0,2);CHECK(eventually([&] {return server.inspect([](const auto& s) {return s.download_stalled;});}));
        server.notify(1,2);server.notify(2,2);CHECK(driver.wait()==8);CHECK(driver.contains("queue_overflow_reoffer_required"));driver.redacted();
        CHECK(server.inspect([](const auto& s) {return s.tasks[1].accepts==0 && s.tasks[2].accepts==0 && s.tasks[1].offered && s.tasks[2].offered;}));
        cfg["queue_capacity"]=3;write_private(root/"config.json",cfg.to_string());
        server.inspect([](auto& s) {s.notify_enabled=true;});server.release_stalls();
        Process again(root,root/"config.json");CHECK(eventually([&] {return completed(server,3);}));again.signal(SIGTERM);CHECK(again.wait()==0);again.redacted();
        CHECK(server.inspect([](const auto& s) {
            std::cout<<"H4 queue_oracle good="<<s.good<<" reason="<<s.failure_reason;
            for(const auto& t:s.tasks)std::cout<<" accepts="<<t.accepts<<" delivered="<<t.delivered<<" downloads="<<t.downloads;
            std::cout<<'\n';
            return s.good && std::all_of(s.tasks.begin(),s.tasks.end(),[](const auto& t){return t.accepts==1 && t.delivered==1;});
        }));
        auto forensic=DurableStateStore::open(root/"state"/"roc-vehicle-state.sqlite3");CHECK(forensic);
        if(forensic)server.inspect([&](const auto& s){
            for(const auto& t:s.tasks) {
                const auto g=forensic.value()->task_generation({t.id,1});
                int seen=0;for(const auto& entry:t.events)if(forensic.value()->status_event(entry.first))++seen;
                CHECK(g && seen==static_cast<int>(t.events.size()));
            }
        });
        std::cout<<"H4 bounded_queue_pressure explicit_exit=8 offered_retained=2 restart_all_3_delivered=1\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void killed_recovery(int kind,int mode) {
    const auto root=temporary_root();{
        JointFixture server(root,kind);server.inspect([&](auto& s) {s.drop_terminal=mode!=2;s.refuse_replay=mode==1;s.stall_download=mode==2;});
        auto cfg=settings(root,server.port());cfg["http_operation_timeout_ms"]=3000;
        if(mode==0)server.inspect([](auto& s){s.tasks[0].expiry=static_cast<std::int64_t>(std::time(nullptr))+2;});
        configure(root,cfg);
        Process first(root,root/"config.json");CHECK(eventually([&] {return server.inspect([&](const auto& s){return mode==2?s.download_stalled:s.terminal_dropped;});}));
        first.signal(SIGKILL);CHECK(first.wait()==128+SIGKILL);first.redacted();
        std::string body,lease,event;StatusEventRecord original;
        auto db=DurableStateStore::open(root/"state"/"roc-vehicle-state.sqlite3");CHECK(db);
        if(mode!=2) {
            const auto pending=db.value()->pending_status_events();CHECK(pending && pending.value().size()==1);
            if(pending && !pending.value().empty()){original=pending.value()[0];body=original.payload_json;lease=original.lease_token;event=original.event_id;}
            CHECK(db.value()->has_unconfirmed_terminal_event(task).value());
        }
        db.value().reset();server.release_stalls();
        if(mode==0)CHECK(eventually([&]{return std::time(nullptr)>original.lease_expires_at;},4000));
        const auto previous=server.inspect([](const auto& s){return s.requests.size();});
        Process restarted(root,root/"config.json");
        if(mode==1) {
            CHECK(restarted.wait()==5);CHECK(restarted.contains("reconciliation_required"));restarted.redacted();
            db=DurableStateStore::open(root/"state"/"roc-vehicle-state.sqlite3");CHECK(db);
            const auto saved=db.value()->status_event(event);CHECK(saved && saved.value().disposition==StatusEventDisposition::reconciliation_required);
            if(saved)CHECK(saved.value().payload_json==body && saved.value().lease_token==lease && saved.value().request_sha256==original.request_sha256 &&
                saved.value().local_delivery_evidence==original.local_delivery_evidence);
            db.value().reset();const auto handshakes=server.inspect([](const auto& s){return s.handshakes;});
            Process blocked(root,root/"config.json");CHECK(blocked.wait()==5);CHECK(blocked.contains("reconciliation_required"));blocked.redacted();
            CHECK(server.inspect([&](const auto& s){return s.handshakes==handshakes && s.tasks[0].accepts==1;}));
            Process resolved(root,root/"config.json",{"--resolve",event,"--confirm-coordination"});CHECK(resolved.wait()==0);resolved.redacted();
            CHECK(server.inspect([&](const auto& s){return s.handshakes==handshakes && s.tasks[0].accepts==1;}));
            db=DurableStateStore::open(root/"state"/"roc-vehicle-state.sqlite3");CHECK(db.value()->status_event(event).value().lease_token.empty());db.value().reset();
            server.reoffer(0);server.inspect([](auto& s){s.refuse_replay=false;});Process next(root,root/"config.json");
            CHECK(eventually([&]{return completed(server);}));next.signal(SIGTERM);CHECK(next.wait()==0);next.redacted();
            CHECK(server.inspect([](const auto& s){return s.good && s.tasks[0].attempt==2 && s.tasks[0].accepts==2 && s.tasks[0].progress==100;}));
        } else {
            CHECK(eventually([&]{return completed(server);}));CHECK(eventually([&]{return restarted.contains("startup_recovery_complete");}));
            restarted.signal(SIGTERM);CHECK(restarted.wait()==0);restarted.redacted();
            CHECK(server.inspect([&](const auto& s){return s.good && s.tasks[0].accepts==1 && s.requests.size()>previous &&
                (mode==2 || s.requests[previous]==task_status_target(task));}));
            if(mode==0)CHECK(server.inspect([&](const auto& s){const auto& t=s.tasks[0];return t.terminal_bodies.size()==2 &&
                t.terminal_bodies[0]==body && t.terminal_bodies[1]==body && t.terminal_leases[0]==lease && t.terminal_leases[1]==lease && t.downloads==(kind==4?2:1);}));
            if(mode==2)CHECK(restarted.contains("active_recovery_no_accept"));
        }
        std::cout<<"H3 actual_driver_SIGKILL resource="<<resource(kind).name<<" window="<<mode<<" pending_first_exact_or_active_original=1\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void publication_gap(int kind,bool after_commit=false) {
    const auto root=temporary_root();{
        JointFixture server(root,kind);configure(root,settings(root,server.port()));
        CHECK(::setenv("ROC_VEHICLE_TEST_KILL_WINDOW",after_commit?"after_commit":"before_insert",1)==0);
        Process probe(root,root/"config.json",{"--run"},ROC_DRIVER_CRASH_PROBE);
        CHECK(::unsetenv("ROC_VEHICLE_TEST_KILL_WINDOW")==0);
        CHECK(probe.wait()==128+SIGKILL);probe.redacted();CHECK(probe.contains("publication_committed"));
        auto db=DurableStateStore::open(root/"state"/"roc-vehicle-state.sqlite3");CHECK(db);
        CHECK(db.value()->pending_status_events().value().size()==(after_commit?1U:0U));
        CHECK(db.value()->task_generation({task,1}).value().state==(after_commit?TaskExecutionState::delivered:TaskExecutionState::delivering));db.value().reset();
        const auto downloads=server.inspect([](const auto& s){return s.tasks[0].downloads;});
        Process next(root,root/"config.json");CHECK(eventually([&]{return completed(server);}));
        CHECK(next.contains(after_commit?"pending_terminal_first":"publication_revalidated"));CHECK(!next.contains("publication_committed"));next.signal(SIGTERM);CHECK(next.wait()==0);next.redacted();
        CHECK(server.inspect([&](const auto& s){return s.good && s.tasks[0].accepts==1 && s.tasks[0].downloads==downloads && s.tasks[0].delivered==1;}));
        std::cout<<"H3 instrumented_driver_SIGKILL after_terminal_commit="<<after_commit<<" resource="<<resource(kind).name<<" verified_publication_reused=1 downloads_on_restart=0\n";
    }
    std::filesystem::remove_all(root);++scenarios;
}
void synthetic_telemetry_probe() {
    const auto root=temporary_root();{
        JointFixture server(root,4);server.inspect([](auto& s){s.notify_enabled=false;});
        configure(root,settings(root,server.port()));
        CHECK(::setenv("ROC_VEHICLE_TEST_KILL_WINDOW","off",1)==0);
        CHECK(::setenv("ROC_VEHICLE_TEST_SYNTHETIC_TELEMETRY","1",1)==0);
        Process probe(root,root/"config.json",{"--run"},ROC_DRIVER_CRASH_PROBE);
        CHECK(eventually([&]{return server.inspect([](const auto& s){return s.telemetries==1 && s.heartbeats>=1;});}));
        probe.signal(SIGTERM);CHECK(probe.wait()==0);probe.redacted();
        CHECK(probe.contains("synthetic_telemetry_queued=1"));
        const auto heartbeats=server.inspect([](const auto& s){return s.heartbeats;});
        Process normal_driver(root,root/"config.json");
        CHECK(eventually([&]{return server.inspect([&](const auto& s){return s.heartbeats>heartbeats;});}));
        normal_driver.signal(SIGTERM);CHECK(normal_driver.wait()==0);normal_driver.redacted();
        CHECK(!normal_driver.contains("test_probe"));
        CHECK(server.inspect([](const auto& s){return s.good && s.telemetries==1;}));
        CHECK(::unsetenv("ROC_VEHICLE_TEST_KILL_WINDOW")==0);
        CHECK(::unsetenv("ROC_VEHICLE_TEST_SYNTHETIC_TELEMETRY")==0);
        std::cout<<"TEST_ONLY synthetic_telemetry_ack_and_Release_ignores_probe_environment=1\n";
    }std::filesystem::remove_all(root);++scenarios;
}
}
int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"--safe";
    if(mode=="--prechecks")prechecks();
    else if(mode=="--safe") {
        for(int kind=0;kind<7;++kind)normal(kind);
        reconnect_timeout_and_restart();security_stops();known_hash_failure();offline_budget();exclusive_stop();pressure();
    } else if(mode=="--faults") {
        synthetic_telemetry_probe();
        for(const int kind:{4,6}) {for(int window=0;window<3;++window)killed_recovery(kind,window);publication_gap(kind);publication_gap(kind,true);}
    } else if(mode=="--context") {
        context_prechecks();
        for(const int kind:{5,6})for(const auto* coord:{"metric","legacy-normalized"})context_delivery(kind,coord,0);
        for(int mismatch=1;mismatch<=5;++mismatch)context_delivery(6,"metric",mismatch);
        context_delivery(5,"metric",5);
        context_publication_recovery();
    } else if(mode=="--pressure")pressure();
    else return 2;
    std::cout<<"H1-H5 joint scenario_groups="<<scenarios<<" failures="<<failures<<" mode="<<mode<<'\n';
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
