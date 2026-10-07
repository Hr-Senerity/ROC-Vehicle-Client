#include "../road_test_support.hpp"
#include "../package_test_support.hpp"
#include "roc/vehicle/road_receiver.hpp"
#include "roc/vehicle/package_receiver.hpp"
#include "roc/vehicle/state_store.hpp"
#include "roc/vehicle/task_protocol.hpp"
#include <sqlite3.h>
#include <csignal>
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <iostream>
#include <sys/wait.h>
#include <unistd.h>

using namespace roc::vehicle;
namespace {
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n';++failures; } } while(false)
constexpr auto task=road_test::task_id;
constexpr auto terminal_id="123e4567-e89b-42d3-a456-426614174043";
constexpr auto lease="synthetic-v08-delivery-lease";
[[noreturn]] void strong_kill() { ::kill(::getpid(),SIGKILL);std::_Exit(99); }
bool killed(pid_t child) { int status=0;return ::waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL; }
StatusEventRecord status_event(TaskExecutionState state,const ArtifactManifest& manifest,std::string evidence) {
    StatusEventRecord e;e.event_id=state==TaskExecutionState::downloading?"123e4567-e89b-42d3-a456-426614174041":
        state==TaskExecutionState::delivering?"123e4567-e89b-42d3-a456-426614174042":terminal_id;
    e.generation={task,1};e.state=state;e.progress=state==TaskExecutionState::delivered?100:
        state==TaskExecutionState::delivering?75:30;
    e.lease_token=lease;e.lease_expires_at=4102444800;e.local_delivery_evidence=std::move(evidence);
    e.payload_json=make_status_request(e).value();EXPECT(e.generation.task_id==manifest.task_id);return e;
}
void active(DurableStateStore& store,const ArtifactManifest& m) {
    EXPECT(store.register_task_generation({task,1},30));
    TaskAvailableMessage n;n.task_id=task;n.batch_id=m.batch_id;n.project_id=m.project_id;
    n.resource_type=m.resource_type;n.resource_revision_id=m.resource_revision_id;n.max_attempts=3;
    EXPECT(store.persist_active_task_lease({n,{task,1},lease,4102444800,30}));
    for (const auto state:{TaskExecutionState::downloading,TaskExecutionState::delivering}) {
        auto e=status_event(state,m,R"({"phase":"before_commit"})");
        EXPECT(store.persist_status_event(e));EXPECT(store.acknowledge_status_event(e.event_id));
    }
}
// Reference application receiver only, not a library-provided generic ROS policy.
// SQLite makes its external effect and stable commit identity atomic together.
class ReceiverJournal {
public:
    explicit ReceiverJournal(const std::filesystem::path& path) {
        const int fd=::open(path.c_str(),O_CREAT|O_EXCL|O_RDWR|O_CLOEXEC|O_NOFOLLOW,0600);
        if (fd>=0) EXPECT(::close(fd)==0);else EXPECT(errno==EEXIST);
        EXPECT(sqlite3_open(path.c_str(),&db_)==SQLITE_OK);
        EXPECT(sqlite3_exec(db_,"PRAGMA synchronous=FULL;CREATE TABLE IF NOT EXISTS commits("
            "task TEXT,attempt INTEGER,revision TEXT,hash TEXT,PRIMARY KEY(task,attempt));",nullptr,nullptr,nullptr)==SQLITE_OK);
    }
    ~ReceiverJournal() { sqlite3_close(db_); }
    Result<void> commit(const ArtifactManifest& m) {
        sqlite3_stmt* q=nullptr;
        if (sqlite3_prepare_v2(db_,"INSERT INTO commits VALUES(?,1,?,?) ON CONFLICT DO NOTHING",-1,&q,nullptr)!=SQLITE_OK)
            return Error{ErrorDomain::storage,ErrorCode::io_failure,false,"synthetic receiver refused"};
        sqlite3_bind_text(q,1,task,-1,SQLITE_STATIC);sqlite3_bind_text(q,2,m.resource_revision_id.c_str(),-1,SQLITE_TRANSIENT);
        sqlite3_bind_text(q,3,m.sha256.c_str(),-1,SQLITE_TRANSIENT);
        const bool saved=sqlite3_step(q)==SQLITE_DONE;sqlite3_finalize(q);
        return saved && committed(m)?Result<void>{}:Result<void>{Error{ErrorDomain::storage,ErrorCode::io_failure,false,"synthetic receiver refused"}};
    }
    bool committed(const ArtifactManifest& m) {
        sqlite3_stmt* q=nullptr;EXPECT(sqlite3_prepare_v2(db_,"SELECT revision,hash FROM commits WHERE task=? AND attempt=1",-1,&q,nullptr)==SQLITE_OK);
        sqlite3_bind_text(q,1,task,-1,SQLITE_STATIC);const bool found=sqlite3_step(q)==SQLITE_ROW;
        bool exact=false;if (found) exact=m.resource_revision_id==reinterpret_cast<const char*>(sqlite3_column_text(q,0)) &&
            m.sha256==reinterpret_cast<const char*>(sqlite3_column_text(q,1));
        sqlite3_finalize(q);return exact;
    }
    int count() { sqlite3_stmt* q=nullptr;EXPECT(sqlite3_prepare_v2(db_,"SELECT COUNT(*) FROM commits",-1,&q,nullptr)==SQLITE_OK);
        EXPECT(sqlite3_step(q)==SQLITE_ROW);const int rows=sqlite3_column_int(q,0);sqlite3_finalize(q);return rows; }
private:sqlite3* db_{nullptr};
};
void gap(const std::filesystem::path& root,bool package,int boundary) {
    const auto road=road_test::fixture().to_string();const auto map=package_test::fixture();
    const auto m=package?map.manifest:road_test::manifest(road);
    const auto db=root/"state"/"state.sqlite3";auto setup=DurableStateStore::open(db);EXPECT(setup);if (!setup) return;
    active(*setup.value(),m);setup.value().reset();
    const auto journal=root/"state"/"receiver.sqlite3";
    {ReceiverJournal external(journal);EXPECT(external.count()==0);}
    const auto artifacts=root/"artifacts";
    const auto delivered=status_event(TaskExecutionState::delivered,m,boundary==2?
        R"({"receiver":"reference_external","commit_identity":"task_attempt_revision_hash"})":
        R"({"receiver":"verified_file","publication_revalidated":true})");
    const pid_t child=::fork();EXPECT(child>=0);
    if (child==0) {
        auto store=DurableStateStore::open(db);if (!store) std::_Exit(10);
        if (package) {
            auto sink=package_test::prepared(artifacts,map);if (!sink) std::_Exit(11);
            auto published=sink.value()->commit();if (!published) std::_Exit(12);
            if (boundary==2) {
                ReceiverJournal external(journal);
                if (!deliver_package_to_verified_callback(published.value(),[&](const DeliveredMapPackage&)->Result<void>{return external.commit(m);})) std::_Exit(13);
            }
        } else {
            auto sink=FileSink::create(artifacts,m,{"metric"});if (!sink) std::_Exit(14);
            if (!sink.value()->write(reinterpret_cast<const std::uint8_t*>(road.data()),road.size()) ||
                !sink.value()->seal(road_test::metadata(m))) std::_Exit(15);
            auto published=sink.value()->commit();if (!published) std::_Exit(16);
            if (boundary==2) {
                ReceiverJournal external(journal);
                if (!deliver_road_network_v2_to_callback(published.value(),[&](const DeliveredRoadNetworkV2&)->Result<void>{return external.commit(m);})) std::_Exit(17);
            }
        }
        if (boundary==1 && !store.value()->persist_status_event(delivered)) std::_Exit(18);
        strong_kill();
    }
    if (child<0) return;
    EXPECT(killed(child));
    auto store=DurableStateStore::open(db);EXPECT(store);if (!store) return;
    auto pending=store.value()->pending_status_events();EXPECT(pending);
    EXPECT(pending.value().size()==(boundary==1?1U:0U));
    if (boundary==1) {
        const auto retained=store.value()->status_event(terminal_id).value();
        EXPECT(retained.payload_json==delivered.payload_json);EXPECT(retained.request_sha256==sha256_hex(delivered.payload_json).value());
        EXPECT(retained.lease_token==lease && retained.local_delivery_evidence==delivered.local_delivery_evidence);
        EXPECT(store.value()->has_unconfirmed_terminal_event(task).value());
        EXPECT(!store.value()->mark_task_generation_expired({task,1}));
    } else {
        ClientConfig config;config.base_url="http://127.0.0.1:1";config.state_directory=root/"state";config.artifact_directory=artifacts;
        auto client=DeviceTaskHttpClient::create(config,[]{return Result<std::string>{"synthetic-gap-token"};});EXPECT(client);
        auto recovered=client.value()->recover_accepted_task({task,1},*store.value());EXPECT(recovered);
        if (recovered) EXPECT(recovered.value().lease_token==lease && recovered.value().progress_floor==75);
        if (package) EXPECT(inspect_map_package(artifacts,m));
        else { auto inspected=inspect_file_artifact(artifacts,m,{"metric"});EXPECT(inspected);
            if (inspected) { EXPECT(road_test::read(inspected.value().path)==road);EXPECT(load_verified_road_network_v2(inspected.value(),{"metric"})); } }
        if (boundary==2) {
            ReceiverJournal external(journal);EXPECT(external.committed(m));EXPECT(external.count()==1);
            auto different=m;different.sha256=std::string(64,'0');EXPECT(!external.commit(different));EXPECT(external.count()==1);
            // Recovery consumes the journal fact, without another callback/effect.
        }
        EXPECT(store.value()->persist_status_event(delivered));
        EXPECT(store.value()->has_unconfirmed_terminal_event(task).value());
    }
    store.value().reset();store=DurableStateStore::open(db);EXPECT(store);
    EXPECT(store.value()->status_event(terminal_id).value().lease_token==lease);
    // Local ACK oracle only: wire replay itself is covered by road_http/package_http.
    EXPECT(store.value()->acknowledge_status_event(terminal_id));
    EXPECT(!store.value()->has_unconfirmed_terminal_event(task).value());
    EXPECT(store.value()->active_task_keys().value().empty());
}
void tamper(const std::filesystem::path& root) {
    const auto text=road_test::fixture().to_string();const auto m=road_test::manifest(text);
    auto sink=FileSink::create(root,m);EXPECT(sink);if (!sink) return;
    EXPECT(sink.value()->write(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
    EXPECT(sink.value()->seal(road_test::metadata(m)));auto published=sink.value()->commit();EXPECT(published);if (!published) return;
    {std::ofstream output(published.value().path,std::ios::binary);output<<"tampered";}
    EXPECT(!inspect_file_artifact(root,m));
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/"roc-v08-delivery-recovery";
    std::error_code error;std::filesystem::remove_all(root,error);
    for (const bool package:{false,true}) for (int point=0;point<3;++point)
        gap(root/(std::string{package?"map-":"road-"}+std::to_string(point)),package,point);
    tamper(root/"tamper");std::filesystem::remove_all(root,error);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
