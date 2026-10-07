#include "roc/vehicle/state_store.hpp"
#include "roc/vehicle/task_protocol.hpp"
#include "roc/vehicle/task_http_client.hpp"
#include "roc/vehicle/crypto.hpp"
#include <sqlite3.h>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

using namespace roc::vehicle;
namespace {
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n';++failures; } } while(false)
constexpr auto task="123e4567-e89b-12d3-a456-426614174010";
constexpr auto event_id="123e4567-e89b-42d3-a456-426614174043";
constexpr auto lease="synthetic-v08-fault-lease";
enum class Action { none,fail_before,fail_after,kill_before,kill_after };
Action action=Action::none;
std::string needle;
bool fired=false;
[[noreturn]] void strong_kill() { ::kill(::getpid(),SIGKILL);std::_Exit(99); }
void arm(std::string match,Action value) { needle=std::move(match);action=value;fired=false; }
bool before() {
    fired=true;
    if (action==Action::kill_before) strong_kill();
    return action==Action::fail_before;
}
int after(int result) {
    const auto saved=action;action=Action::none;
    if (result==SQLITE_DONE || result==SQLITE_OK) {
        if (saved==Action::kill_after) strong_kill();
        if (saved==Action::fail_after) return SQLITE_IOERR_WRITE;
    }
    return result;
}
ActiveTaskLease binding() {
    TaskAvailableMessage n;n.task_id=task;n.batch_id="123e4567-e89b-12d3-a456-426614174011";
    n.project_id="123e4567-e89b-12d3-a456-426614174012";n.resource_type=ResourceType::road_network;
    n.resource_revision_id="123e4567-e89b-12d3-a456-426614174013";n.max_attempts=3;
    return {n,{task,1},lease,4102444800,20};
}
StatusEventRecord event(TaskExecutionState state,std::string id=event_id) {
    StatusEventRecord e;e.event_id=std::move(id);e.generation={task,1};e.state=state;
    e.progress=state==TaskExecutionState::delivered?100:state==TaskExecutionState::delivering?80:20;
    if (state==TaskExecutionState::failed) e.error_code="local_storage_failure";
    e.lease_token=lease;e.lease_expires_at=4102444800;
    e.local_delivery_evidence=R"({"receiver":"verified_file","schema_version":2,"synthetic":true})";
    e.payload_json=make_status_request(e).value();return e;
}
void prepare(DurableStateStore& store,TaskExecutionState state) {
    EXPECT(store.register_task_generation({task,1},20));EXPECT(store.persist_active_task_lease(binding()));
    if (state==TaskExecutionState::delivering || state==TaskExecutionState::delivered) {
        auto down=event(TaskExecutionState::downloading,"123e4567-e89b-42d3-a456-426614174041");
        EXPECT(store.persist_status_event(down));EXPECT(store.acknowledge_status_event(down.event_id));
    }
    if (state==TaskExecutionState::delivered) {
        auto delivering=event(TaskExecutionState::delivering,"123e4567-e89b-42d3-a456-426614174042");
        EXPECT(store.persist_status_event(delivering));EXPECT(store.acknowledge_status_event(delivering.event_id));
    }
}
void sql(const std::filesystem::path& db,const std::string& text) {
    sqlite3* raw=nullptr;EXPECT(sqlite3_open(db.c_str(),&raw)==SQLITE_OK);
    EXPECT(sqlite3_exec(raw,text.c_str(),nullptr,nullptr,nullptr)==SQLITE_OK);EXPECT(sqlite3_close(raw)==SQLITE_OK);
}
int scalar(const std::filesystem::path& db,const char* text) {
    sqlite3* raw=nullptr;sqlite3_stmt* q=nullptr;
    EXPECT(sqlite3_open_v2(db.c_str(),&raw,SQLITE_OPEN_READONLY,nullptr)==SQLITE_OK);
    EXPECT(sqlite3_prepare_v2(raw,text,-1,&q,nullptr)==SQLITE_OK);
    EXPECT(sqlite3_step(q)==SQLITE_ROW);const int value=sqlite3_column_int(q,0);
    sqlite3_finalize(q);sqlite3_close(raw);return value;
}
StatusEventRecord legacy(const std::filesystem::path& db,int version,TaskExecutionState state,bool confirmed=false) {
    auto store=DurableStateStore::open(db,4);EXPECT(store);if (!store) std::abort();
    prepare(*store.value(),state);auto e=event(state);EXPECT(store.value()->persist_status_event(e));
    if (confirmed) EXPECT(store.value()->acknowledge_status_event(e.event_id));
    auto original=store.value()->status_event(e.event_id).value();EXPECT(store.value()->allocate_sequence());store.value().reset();
    sql(db,"DROP TRIGGER clear_active_terminal_lease;DROP TRIGGER clear_expired_active_lease;"
        "DROP TABLE active_task_leases;DELETE FROM schema_migrations WHERE version=3;");
    if (version==1) {
        sql(db,"ALTER TABLE status_events DROP COLUMN request_sha256;"
            "ALTER TABLE status_events DROP COLUMN lease_token;ALTER TABLE status_events DROP COLUMN lease_expires_at;"
            "ALTER TABLE status_events DROP COLUMN local_delivery_evidence;ALTER TABLE status_events DROP COLUMN disposition;"
            "ALTER TABLE status_events DROP COLUMN reconciliation_reason;DELETE FROM schema_migrations WHERE version=2;");
    }
    return original;
}
bool killed(pid_t child) {
    int status=0;return ::waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL;
}
void classification(const std::filesystem::path& base) {
    int index=0;
    for (const auto state:{TaskExecutionState::downloading,TaskExecutionState::delivering,TaskExecutionState::delivered,TaskExecutionState::failed})
        for (const int version:{1,2}) for (const bool confirmed:{false,true}) {
            const auto db=base/std::to_string(index++)/"state.sqlite3";const auto original=legacy(db,version,state,confirmed);
            auto store=DurableStateStore::open(db,4);EXPECT(store);if (!store) continue;
            const auto row=store.value()->status_event(event_id).value();const bool terminal=state==TaskExecutionState::delivered || state==TaskExecutionState::failed;
            EXPECT(row.payload_json==original.payload_json);EXPECT(store.value()->reserved_through()==4);
            EXPECT(store.value()->active_task_keys().value().empty());
            if (confirmed) { EXPECT(row.disposition==StatusEventDisposition::acknowledged);EXPECT(row.lease_token.empty()); }
            else if (version==1) {
                EXPECT(row.disposition==(terminal?StatusEventDisposition::reconciliation_required:StatusEventDisposition::forensic));
                EXPECT(row.lease_token.empty());EXPECT(row.request_sha256.empty());
                EXPECT(store.value()->task_generation({task,1}).value().state==
                    (terminal?TaskExecutionState::reconciliation_required:TaskExecutionState::expired));
            } else {
                EXPECT(row.disposition==original.disposition);EXPECT(row.lease_token==original.lease_token);
                EXPECT(row.request_sha256==original.request_sha256);EXPECT(row.local_delivery_evidence==original.local_delivery_evidence);
                EXPECT(row.lease_expires_at==original.lease_expires_at);
                EXPECT(store.value()->has_unrecoverable_active_generation(task).value()==!terminal);
            }
            EXPECT(store.value()->has_unconfirmed_terminal_event(task).value()==(!confirmed && terminal));
        }
}
void migration_faults(const std::filesystem::path& base) {
    int index=0;
    // Migration 2 and 3 are individually atomic; v1->v3 can retain a complete v2 checkpoint.
    for (const std::string checkpoint:{"ALTER TABLE status_events ADD COLUMN request_sha256", "CREATE TABLE active_task_leases",
             "CREATE TRIGGER clear_active_terminal_lease", "CREATE TRIGGER clear_expired_active_lease", "VALUES(3,", "COMMIT"})
        for (const auto fault:{Action::fail_before,Action::fail_after,Action::kill_before,Action::kill_after}) {
            const bool second=checkpoint.find("ALTER")==0;const auto db=base/std::to_string(index++)/"state.sqlite3";
            const auto original=legacy(db,second?1:2,TaskExecutionState::failed);
            if (fault==Action::kill_before || fault==Action::kill_after) {
                const pid_t child=::fork();EXPECT(child>=0);if (child==0) { arm(checkpoint,fault);static_cast<void>(DurableStateStore::open(db));std::_Exit(80); }
                if (child>=0) EXPECT(killed(child));
            } else {
                arm(checkpoint,fault);EXPECT(!DurableStateStore::open(db));EXPECT(fired);action=Action::none;
            }
            const int expected=second?1:(checkpoint=="COMMIT" && (fault==Action::fail_after || fault==Action::kill_after)?3:2);
            EXPECT(scalar(db,"SELECT MAX(version) FROM schema_migrations")==expected);
            EXPECT(scalar(db,"SELECT COUNT(*) FROM sqlite_master WHERE name='active_task_leases'")==(expected==3?1:0));
            auto store=DurableStateStore::open(db);EXPECT(store);if (!store) continue;
            EXPECT(scalar(db,"SELECT MAX(version) FROM schema_migrations")==3);
            const auto row=store.value()->status_event(event_id).value();EXPECT(row.payload_json==original.payload_json);
            if (second) EXPECT(row.disposition==StatusEventDisposition::reconciliation_required);
            else { EXPECT(row.lease_token==original.lease_token);EXPECT(row.request_sha256==original.request_sha256); }
            EXPECT(store.value()->has_unconfirmed_terminal_event(task).value());
        }
}
enum class Operation { generation,active,downloading,delivering,nonterminal_ack,expire,terminal,terminal_ack,reconcile,reconcile_commit,resolve };
void operation_faults(const std::filesystem::path& base) {
    int index=0;
    for (const auto op:{Operation::generation,Operation::active,Operation::downloading,Operation::delivering,Operation::nonterminal_ack,Operation::expire,
                        Operation::terminal,Operation::terminal_ack,Operation::reconcile,Operation::reconcile_commit,Operation::resolve})
        for (const auto fault:{Action::fail_before,Action::fail_after,Action::kill_before,Action::kill_after}) {
            const auto db=base/std::to_string(index++)/"state.sqlite3";
            auto setup=DurableStateStore::open(db);EXPECT(setup);if (!setup) continue;
            if (op==Operation::generation) { /* Open alone; accepted row is the fault target. */ }
            else if (op==Operation::active) EXPECT(setup.value()->register_task_generation({task,1},20));
            else if (op==Operation::terminal || op==Operation::delivering) prepare(*setup.value(),op==Operation::terminal?TaskExecutionState::delivered:TaskExecutionState::delivering);
            else prepare(*setup.value(),TaskExecutionState::downloading);
            if (op==Operation::nonterminal_ack) EXPECT(setup.value()->persist_status_event(event(TaskExecutionState::downloading)));
            if (op==Operation::terminal_ack || op==Operation::reconcile || op==Operation::reconcile_commit || op==Operation::resolve) {
                EXPECT(setup.value()->persist_status_event(event(TaskExecutionState::failed)));
                if (op==Operation::resolve) EXPECT(setup.value()->mark_status_event_reconciliation_required(event_id,"invalid_lease"));
            }
            setup.value().reset();
            std::string match=op==Operation::generation?"INSERT INTO task_generations":op==Operation::active?"INSERT INTO active_task_leases":
                op==Operation::expire?"UPDATE task_generations SET state = 'expired'":
                op==Operation::terminal_ack || op==Operation::nonterminal_ack?"disposition = 'acknowledged'":
                op==Operation::resolve?"disposition = 'resolved'":
                op==Operation::reconcile?"SET disposition = 'reconciliation_required'":"COMMIT";
            const auto execute_op=[&]() {
                auto store=DurableStateStore::open(db);if (!store) std::abort();arm(match,fault);
                switch(op) {
                    case Operation::generation: {
                        auto saved=store.value()->register_task_generation({task,1},20);
                        return saved?Result<void>{}:Result<void>{saved.error()};
                    }
                    case Operation::active:return store.value()->persist_active_task_lease(binding());
                    case Operation::expire:return store.value()->mark_task_generation_expired({task,1});
                    case Operation::nonterminal_ack:case Operation::terminal_ack:return store.value()->acknowledge_status_event(event_id);
                    case Operation::reconcile:case Operation::reconcile_commit:return store.value()->mark_status_event_reconciliation_required(event_id,"invalid_lease");
                    case Operation::resolve:return store.value()->resolve_status_event_reconciliation(event_id);
                    default:return store.value()->persist_status_event(event(op==Operation::terminal?TaskExecutionState::delivered:
                        op==Operation::delivering?TaskExecutionState::delivering:TaskExecutionState::downloading));
                }
            };
            if (fault==Action::kill_before || fault==Action::kill_after) {
                const pid_t child=::fork();EXPECT(child>=0);if (child==0) {static_cast<void>(execute_op());std::_Exit(81);}
                if (child>=0) EXPECT(killed(child));
            } else { EXPECT(!execute_op());EXPECT(fired);action=Action::none; }
            auto store=DurableStateStore::open(db);EXPECT(store);if (!store) continue;
            bool committed=fault==Action::fail_after || fault==Action::kill_after;
            // Reconciliation's first UPDATE is still inside an uncommitted transaction.
            if (op==Operation::reconcile) committed=false;
            const int active=scalar(db,"SELECT COUNT(*) FROM active_task_leases");
            EXPECT(active==(op==Operation::generation?0:op==Operation::active?(committed?1:0):
                (op==Operation::expire || op==Operation::terminal_ack || op==Operation::resolve) && committed?0:1));
            if (op==Operation::generation) EXPECT(scalar(db,"SELECT COUNT(*) FROM task_generations")== (committed?1:0));
            if (op==Operation::expire) EXPECT(store.value()->task_generation({task,1}).value().state==
                (committed?TaskExecutionState::expired:TaskExecutionState::accepted));
            if (op==Operation::terminal || op==Operation::downloading || op==Operation::delivering) {
                auto row=store.value()->status_event(event_id);EXPECT(static_cast<bool>(row)==committed);
                const auto state=store.value()->task_generation({task,1}).value().state;
                EXPECT(state==(committed?event(op==Operation::terminal?TaskExecutionState::delivered:
                    op==Operation::delivering?TaskExecutionState::delivering:TaskExecutionState::downloading).state:
                    op==Operation::terminal?TaskExecutionState::delivering:op==Operation::delivering?TaskExecutionState::downloading:TaskExecutionState::accepted));
                if (row) { EXPECT(row.value().payload_json==event(row.value().state).payload_json);EXPECT(row.value().lease_token==lease); }
            }
            if (op==Operation::terminal_ack || op==Operation::resolve || op==Operation::reconcile || op==Operation::reconcile_commit) {
                const auto row=store.value()->status_event(event_id).value();EXPECT(row.local_delivery_evidence==event(TaskExecutionState::failed).local_delivery_evidence);
                const bool released=committed && (op==Operation::terminal_ack || op==Operation::resolve);
                EXPECT(row.payload_json==event(TaskExecutionState::failed).payload_json);
                EXPECT(row.request_sha256==sha256_hex(row.payload_json).value());EXPECT(row.lease_expires_at==4102444800);
                EXPECT(row.lease_token==(released?"":lease));
                EXPECT(store.value()->has_unconfirmed_terminal_event(task).value()==!released);
                if (op==Operation::reconcile_commit) EXPECT(row.disposition==
                    (committed?StatusEventDisposition::reconciliation_required:StatusEventDisposition::pending));
            }
            if (op==Operation::nonterminal_ack) {
                EXPECT(store.value()->active_task_lease({task,1}).value().lease_token==lease);
                EXPECT(store.value()->status_event(event_id).value().disposition==
                    (committed?StatusEventDisposition::acknowledged:StatusEventDisposition::pending));
            }
            ClientConfig config;config.base_url="http://127.0.0.1:1";
            config.state_directory=db.parent_path();config.artifact_directory=db.parent_path()/"artifacts";
            auto client=DeviceTaskHttpClient::create(config,[]{return Result<std::string>{"synthetic-v08-fault-token"};});EXPECT(client);
            if (active==1 && (op==Operation::active || op==Operation::downloading || op==Operation::delivering || op==Operation::nonterminal_ack)) {
                auto recovered=client.value()->recover_accepted_task({task,1},*store.value());EXPECT(recovered);
                if (recovered) EXPECT(recovered.value().lease_token==lease && recovered.value().progress_floor>=20);
            }
            if (store.value()->has_unconfirmed_terminal_event(task).value()) {
                auto refused=client.value()->accept_task(binding().notification,*store.value());EXPECT(!refused);
                if (!refused) EXPECT(refused.error().server_code=="terminal_outbox_pending");
            }
        }
}
}
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __real_sqlite3_exec(sqlite3*,const char*,int(*)(void*,int,char**,char**),void*,char**);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    const auto* sql_text=sqlite3_sql(statement);
    if (action==Action::none || !sql_text || std::string_view{sql_text}.find(needle)==std::string_view::npos)
        return __real_sqlite3_step(statement);
    if (before()) {action=Action::none;return SQLITE_IOERR_WRITE;}
    return after(__real_sqlite3_step(statement));
}
extern "C" int __wrap_sqlite3_exec(sqlite3* db,const char* text,int(*callback)(void*,int,char**,char**),void* data,char** error) {
    if (action==Action::none || callback || std::string_view{text}.find(needle)==std::string_view::npos)
        return __real_sqlite3_exec(db,text,callback,data,error);
    // Split with SQLite's parser (including whole trigger bodies), not on ';'.
    const char* tail=text;
    while (*tail) {
        sqlite3_stmt* statement=nullptr;const char* next=nullptr;
        const int parsed=sqlite3_prepare_v2(db,tail,-1,&statement,&next);
        if (parsed!=SQLITE_OK) return parsed;
        if (!statement) {tail=next;continue;}
        const std::string part=sqlite3_sql(statement);sqlite3_finalize(statement);
        const bool match=action!=Action::none && part.find(needle)!=std::string::npos;
        if (match && before()) {action=Action::none;return SQLITE_IOERR_WRITE;}
        const int result=__real_sqlite3_exec(db,part.c_str(),nullptr,nullptr,error);
        const int injected=match?after(result):result;
        if (injected!=SQLITE_OK) return injected;
        tail=next;
    }
    return SQLITE_OK;
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/"roc-v08-state-faults";
    std::error_code error;std::filesystem::remove_all(root,error);
    classification(root/"classification");migration_faults(root/"migration");operation_faults(root/"operation");
    std::filesystem::remove_all(root,error);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
