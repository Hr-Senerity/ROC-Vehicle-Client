#include "driver_support.hpp"
#include "private_paths.hpp"
#include "roc/vehicle/device_client.hpp"
#include "roc/vehicle/file_sink.hpp"
#include "roc/vehicle/package_sink.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include "roc/vehicle/crypto.hpp"
#include <jsoncons/json.hpp>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <csignal>
#include <ctime>
#include <deque>
#include <iostream>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#ifdef ROC_VEHICLE_TEST_INSTRUMENTATION
// Defined only by the separately identified integration probe, not Release.
void roc_vehicle_test_online(roc::vehicle::DeviceClient&);
#endif

namespace {
using namespace roc::vehicle;
using namespace roc::vehicle::driver;
using Json = jsoncons::json;
volatile std::sig_atomic_t interrupted = 0;
void signal_stop(int) { interrupted = 1; }
std::mutex output_mutex;
void record(std::string_view code, const TaskGenerationKey* key = nullptr) {
    // Only program-defined codes and a task alias, never response prose/config.
    std::lock_guard<std::mutex> guard(output_mutex);
    std::cout << "driver code=" << code;
    if (key) {
        const auto hash = sha256_hex(key->task_id);
        std::cout << " task=" << (hash ? hash.value().substr(0,12) : "unknown") << " attempt=" << key->attempt;
    }
    std::cout << '\n' << std::flush;
}
void error_record(const Error& e) {
    std::lock_guard<std::mutex> guard(output_mutex);
    // Numeric core classification is stable, safe even for unknown response codes.
    std::cout << "driver code=operation_error domain=" << static_cast<int>(e.domain)
              << " error=" << static_cast<int>(e.code) << " retryable=" << e.retryable << '\n' << std::flush;
}
bool terminal(TaskExecutionState s) { return s == TaskExecutionState::delivered || s == TaskExecutionState::failed; }
bool confirmed_or_retired(TaskExecutionState s) {
    return terminal(s) || s == TaskExecutionState::expired || s == TaskExecutionState::canceled ||
           s == TaskExecutionState::reconciliation_required;
}
class Coordinator {
public:
    Coordinator(Settings s, std::unique_ptr<DurableStateStore> state,
                std::unique_ptr<DeviceTaskHttpClient> http)
        : settings_(std::move(s)), state_(std::move(state)), http_(std::move(http)) {}
    int run() {
        const auto reconciliations = state_->reconciliation_required_events();
        if (!reconciliations) { record("state_read_failed"); return 5; }
        if (!reconciliations.value().empty()) { record("reconciliation_required"); return 5; }
        DeviceClientCallbacks callbacks;
        callbacks.on_state_changed = [this](ConnectionState s) {
            connection_.store(s);
            if (s == ConnectionState::authentication_stopped) fail(4);
            if (s == ConnectionState::connection_replaced) fail(7);
            if (s == ConnectionState::halted) fail(5);
            condition_.notify_all();
        };
        callbacks.on_error = [this](const Error& e) {
            if (e.domain == ErrorDomain::authentication && !e.retryable) fail(4);
            if (e.domain == ErrorDomain::protocol && !e.retryable && e.server_code.empty()) fail(5);
        };
        callbacks.on_task_available = [this](const TaskAvailableMessage& n) {
            std::lock_guard<std::mutex> guard(mutex_);
            if (stopping_ || exit_code_.load()) return;
            const auto same = [&](const TaskAvailableMessage& other) {
                return other.task_id == n.task_id && other.attempt == n.attempt;
            };
            if ((working_ && same(*working_)) || std::any_of(queue_.begin(), queue_.end(), same)) return;
            if (queue_.size() >= settings_.client.task_queue_capacity) {
                // Fail visibly and reconnect on a later explicit run. The task remains
                // offered at SYSTEM; no HTTP accept/ACK or silent eviction occurs.
                fail(8); return;
            }
            queue_.push_back(n); condition_.notify_all();
        };
        auto ws_config = settings_.client;
        // One working slot plus one overflow slot lets the application report it;
        // both queues remain bounded. Higher attempt of an in-flight task is distinct.
        ws_config.task_queue_capacity += 2;
        auto device = DeviceClient::create(ws_config, provider(), std::move(callbacks));
        if (!device) { error_record(device.error()); return 5; }
        device_ = std::move(device.value());
        worker_ = std::thread([this] {
            try { work(); } catch (...) { record("worker_exception"); fail(5); }
        });
        auto started = device_->start();
        if (!started) { error_record(started.error()); fail(5); }
        auto offline_since = std::chrono::steady_clock::now();
        auto last = ConnectionState::stopped;
        while (!interrupted && exit_code_.load() == 0) {
            const auto state = connection_.load();
            if (state != last) {
                if (state == ConnectionState::online) {
                    record("ws_online");
#ifdef ROC_VEHICLE_TEST_INSTRUMENTATION
                    roc_vehicle_test_online(*device_);
#endif
                }
                if (state == ConnectionState::backing_off) record("ws_backoff");
                last = state;
            }
            if (state == ConnectionState::online) offline_since = std::chrono::steady_clock::now();
            if (std::chrono::steady_clock::now() - offline_since > std::chrono::milliseconds{settings_.offline_budget_ms}) {
                fail(9); break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds{10});
        }
        {
            std::lock_guard<std::mutex> guard(mutex_); stopping_ = true;
        }
        cancellation_.cancel(); condition_.notify_all();
        // Stop DeviceClient outside callbacks; no joining one's own I/O thread.
        device_->stop();
        if (worker_.joinable()) worker_.join();
        const int code = exit_code_.load();
        record(code == 4 ? "authentication_stopped" : code == 7 ? "connection_replaced" :
               code == 8 ? "queue_overflow_reoffer_required" : code == 9 ? "offline_budget_exhausted" :
               code ? "attention_required" : "stopped_evidence_retained");
        return code;
    }
private:
    DeviceTokenProvider provider() {
        const auto path = settings_.token_file;
        return [path] { return read_token(path); };
    }
    void fail(int code) { int zero=0; exit_code_.compare_exchange_strong(zero,code); condition_.notify_all(); }
    bool stop_requested() const { return cancellation_.is_canceled() || interrupted || exit_code_.load() != 0; }
    bool pause(int milliseconds) {
        std::unique_lock<std::mutex> guard(mutex_);
        condition_.wait_for(guard, std::chrono::milliseconds{milliseconds}, [this] { return stop_requested() || stopping_; });
        return !stop_requested() && !stopping_;
    }
    template<class Operation> auto bounded(Operation operation, bool exact_status=false) -> decltype(operation()) {
        auto result = operation(); int delay = settings_.retry_initial_ms;
        for (int tries=1; !result && tries<settings_.retry_attempts && !stop_requested(); ++tries) {
            const auto& e = result.error();
            if (e.domain == ErrorDomain::authentication && !e.retryable) { fail(4); break; }
            if (exact_status) {
                const auto required = state_->reconciliation_required_events();
                if (!required || !required.value().empty() || e.domain == ErrorDomain::lease) break;
            } else if (!e.retryable) break;
            record(exact_status ? "exact_replay_retry" : "operation_retry");
            if (!pause(delay)) break;
            delay = std::min(delay*2,settings_.retry_maximum_ms); result = operation();
        }
        if (!result && result.error().domain == ErrorDomain::authentication && !result.error().retryable) fail(4);
        return result;
    }
    bool replay_pending() {
        auto events = state_->pending_status_events();
        if (!events) { error_record(events.error()); return false; }
        std::stable_sort(events.value().begin(),events.value().end(),[](const auto& a,const auto& b) {
            return terminal(a.state) && !terminal(b.state);
        });
        for (const auto& event : events.value()) {
            if (stop_requested()) return false;
            record(terminal(event.state)?"pending_terminal_first":"pending_nonterminal",&event.generation);
            const auto replay = bounded([&] { return http_->replay_status_event(event,*state_,&cancellation_); },true);
            if (!replay) {
                error_record(replay.error());
                const auto saved = state_->status_event(event.event_id);
                if (saved && saved.value().disposition == StatusEventDisposition::reconciliation_required) record("reconciliation_required",&event.generation);
                else record("pending_retained",&event.generation);
                return false;
            }
            record("event_confirmed",&event.generation);
        }
        return true;
    }
    Result<void> status(const AcceptedTask& task,TaskExecutionState target,std::int64_t progress,
                        std::string evidence,const char* failure=nullptr) {
        if (task.is_expired_at(static_cast<std::int64_t>(std::time(nullptr))))
            return Error{ErrorDomain::lease,ErrorCode::invalid_state,false,"active lease expired","lease_expired"};
        const auto uuid = generate_uuid_v4(); if (!uuid) return uuid.error();
        StatusEventRecord event; event.event_id=uuid.value(); event.generation={task.task_id,task.attempt};
        event.state=target; event.progress=progress; event.lease_token=task.lease_token;
        event.lease_expires_at=task.lease_expires_at; event.local_delivery_evidence=std::move(evidence);
        if (failure) event.error_code=failure;
        const auto body = make_status_request(event); if (!body) return body.error(); event.payload_json=body.value();
        const auto digest = sha256_hex(event.payload_json); if (!digest) return digest.error(); event.request_sha256=digest.value();
        const auto persisted=state_->persist_status_event(event); if (!persisted) return persisted.error();
        record(terminal(target)?"terminal_saved":"status_saved",&event.generation);
        const auto sent=bounded([&] { return http_->report_status(event,task,*state_,&cancellation_); },true);
        if (!sent) {
            const auto saved=state_->status_event(event.event_id);
            if (saved && saved.value().disposition==StatusEventDisposition::reconciliation_required) record("reconciliation_required",&event.generation);
            else record("pending_retained",&event.generation);
            return sent.error();
        }
        record(target==TaskExecutionState::delivered?"delivered_confirmed":target==TaskExecutionState::failed?"failed_confirmed":"status_confirmed",&event.generation);
        return {};
    }
    Result<ArtifactManifest> manifest(const TaskAvailableMessage& n,const AcceptedTask& a) {
        const auto path=manifest_path(settings_,{a.task_id,a.attempt});
        std::error_code ec;
        if (std::filesystem::exists(std::filesystem::symlink_status(path,ec))) {
            const auto bytes=read_private_file(path,65536); if (!bytes) return bytes.error();
            return parse_manifest_response(bytes.value(),n,settings_.client.artifact_limits);
        }
        const auto m=bounded([&] { return http_->read_manifest(n,a,*state_,&cancellation_); });
        if (!m) return m.error();
        const auto text=manifest_document(m.value());
        const auto validated=parse_manifest_response(text,n,settings_.client.artifact_limits); if (!validated) return validated.error();
        const auto saved=save_immutable(path,text); if (!saved) return saved.error();
        return m.value();
    }
    Result<std::string> save_resource(const ArtifactManifest& m,const AcceptedTask& a) {
        const TaskGenerationKey key{a.task_id,a.attempt};
        const auto mode=bind_owning_map(settings_,m,key); if (!mode) return mode.error();
        FileSinkOptions options; options.expected_coordinate_mode=mode.value();
        const auto root=settings_.client.artifact_directory/("task-"+a.task_id+"-"+std::to_string(a.attempt));
        if (!detail::private_directory(root,true)) return make_resource_failure(ResourceFailure::local_storage_failure);
        const auto parts=recover_file_sink_parts(root); if (!parts) return parts.error();
        const auto staging=recover_package_staging(root); if (!staging) return staging.error();
        // A recognized final is immutable. A corrupt final is not overwritten.
        bool published=false;
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(root,ec)) {
            const auto name=entry.path().filename().string();
            if (name.rfind("map-package-",0)==0 || name.rfind("map-v",0)==0 || name.rfind("road-network-v",0)==0) published=true;
        }
        if (ec) return make_resource_failure(ResourceFailure::local_storage_failure);
        if (m.resource_type==ResourceType::road_network)
            record(mode.value().empty()?"owning_map_context_unverified":"owning_map_context_bound",&key);
        auto generation=state_->task_generation(key); if (!generation) return generation.error();
        if (published) {
            if (generation.value().state!=TaskExecutionState::delivering)
                return local_error("publication_state_conflict",ErrorDomain::storage);
            record("publication_revalidated",&key);
            if (m.package_version==2) {
                const auto existing=inspect_map_package(root,m); if (!existing) return existing.error();
                return make_map_package_evidence(existing.value(),key);
            }
            const auto existing=inspect_file_artifact(root,m,options); if (!existing) return existing.error();
            return file_evidence(existing.value(),key,mode.value());
        }
        for (int tries=0; tries<settings_.retry_attempts; ++tries) {
            if (stop_requested()) return local_error("stopped",ErrorDomain::canceled);
            // Each transfer restart uses a fresh sink; no Range, partial bytes or
            // replacement event IDs are reused as an already validated artifact.
            const auto result=download_commit(root,m,a,options);
            if (result) return result;
            const auto pending=state_->pending_status_events();
            if (!pending) return pending.error();
            if (std::any_of(pending.value().begin(),pending.value().end(),[&](const auto& event) {
                return event.generation.task_id==a.task_id;
            })) return result.error();
            if (!result.error().retryable || tries+1==settings_.retry_attempts) return result.error();
            record("download_retry",&key);
            if (!pause(std::min(settings_.retry_initial_ms*(1<<tries),settings_.retry_maximum_ms))) return result.error();
        }
        return local_error("retry_exhausted");
    }
    Result<std::string> file_evidence(const DeliveredArtifact& file,const TaskGenerationKey& key,std::string_view mode) {
        Json j(jsoncons::json_object_arg); j["evidence_version"]=1; j["receiver"]="file_sink";
        j["task_id"]=key.task_id; j["attempt"]=key.attempt; j["resource_revision_id"]=file.resource_revision_id;
        j["sha256"]=file.sha256; j["byte_size"]=file.byte_size; j["file"]=file.path.filename().string(); j["durable"]=true;
        if (!mode.empty()) {
            const auto context=read_private_file(settings_.client.state_directory / ("driver-map-context-"+key.task_id+"-"+
                std::to_string(key.attempt)+".json"),65536);
            // bind_owning_map has durably saved this exact generation before transfer.
            if (!context) return context.error();
            const auto digest=sha256_hex(context.value()); if (!digest) return digest.error();
            j["owning_map_context_sha256"]=digest.value();
            j["owning_map_coordinate_mode"]=std::string(mode);
        }
        return j.to_string();
    }
    Result<std::string> download_commit(const std::filesystem::path& root,const ArtifactManifest& m,const AcceptedTask& a,
                                        const FileSinkOptions& options) {
        const TaskGenerationKey key{a.task_id,a.attempt};
        if (m.package_version==2) {
            auto sink=PackageSink::create(root,m,settings_.client.artifact_limits); if (!sink) return sink.error();
            for (const auto& file : m.files) {
                const auto download=http_->download_artifact_file(m,file.id,a,*state_,[&](const auto* data,std::size_t size) {
                    return sink.value()->write(file.id,data,size);
                },&cancellation_);
                if (!download) return download.error();
                const auto sealed=sink.value()->seal_file(file.id,download.value()); if (!sealed) return sealed.error();
            }
            const auto sealed=sink.value()->seal(); if (!sealed) return sealed.error();
            const auto ready=delivering(a); if (!ready) return ready.error();
            if (stop_requested()) return local_error("stopped",ErrorDomain::canceled);
            const auto saved=sink.value()->commit(); if (!saved) return saved.error();
            record("publication_committed",&key); return make_map_package_evidence(saved.value(),key);
        }
        auto sink=FileSink::create(root,m,options); if (!sink) return sink.error();
        const auto download=http_->download_artifact(m,a,*state_,[&](const auto* data,std::size_t size) {
            return sink.value()->write(data,size);
        },&cancellation_);
        if (!download) return download.error();
        const auto sealed=sink.value()->seal(download.value()); if (!sealed) return sealed.error();
        const auto ready=delivering(a); if (!ready) return ready.error();
        if (stop_requested()) return local_error("stopped",ErrorDomain::canceled);
        const auto saved=sink.value()->commit(); if (!saved) return saved.error();
        record("publication_committed",&key); return file_evidence(saved.value(),key,options.expected_coordinate_mode);
    }
    Result<void> delivering(const AcceptedTask& a) {
        const auto generation=state_->task_generation({a.task_id,a.attempt}); if (!generation) return generation.error();
        if (generation.value().state==TaskExecutionState::delivering) return {};
        return status(a,TaskExecutionState::delivering,std::max<std::int64_t>(generation.value().progress,75),R"({"phase":"verified_before_publication"})");
    }
    Result<void> process(const TaskAvailableMessage& n,std::optional<AcceptedTask> recovered={}) {
        const auto blocked=state_->has_unconfirmed_terminal_event(n.task_id); if (!blocked) return blocked.error();
        if (blocked.value()) return local_error("terminal_pending",ErrorDomain::storage);
        AcceptedTask a;
        if (recovered) a=*recovered;
        else {
            // A previously completed notification is not a platform reoffer.
            // A real reoffer carries the previous accepted attempt, not old zero.
            if (n.attempt < std::numeric_limits<std::int64_t>::max()) {
                const auto old=state_->task_generation({n.task_id,n.attempt+1});
                if (old && confirmed_or_retired(old.value().state)) { record("duplicate_completed"); return {}; }
            }
            const auto keys=state_->active_task_keys(); if (!keys) return keys.error();
            for (const auto& key : keys.value()) if (key.task_id==n.task_id) {
                const auto active=http_->recover_accepted_task(key,*state_); if (!active) return active.error();
                const auto lease=state_->active_task_lease(key); if (!lease) return lease.error();
                return process(lease.value().notification,active.value());
            }
            const auto accepted=bounded([&] { return http_->accept_task(n,*state_,&cancellation_); });
            if (!accepted) return accepted.error();
            a=accepted.value();
            const auto accepted_key=TaskKey(a);
            record("accepted",&accepted_key);
        }
        const TaskGenerationKey key{a.task_id,a.attempt};
        const auto generation=state_->task_generation(key); if (!generation) return generation.error();
        if (generation.value().state==TaskExecutionState::accepted) {
            const auto down=status(a,TaskExecutionState::downloading,std::max<std::int64_t>(a.progress_floor,30),R"({"phase":"download_start"})");
            if (!down) return down.error();
        }
        const auto m=manifest(n,a);
        if (!m) return resource_error(a,m.error(),false);
        const auto delivered=save_resource(m.value(),a);
        if (!delivered) return resource_error(a,delivered.error(),true);
        // Even if stop arrives after the durable commit, preserve terminal facts;
        // cancellation can prevent send but cannot erase this Outbox event.
        return status(a,TaskExecutionState::delivered,100,delivered.value());
    }
    static TaskGenerationKey TaskKey(const AcceptedTask& a) { return {a.task_id,a.attempt}; }
    Result<void> resource_error(const AcceptedTask& a,const Error& e,bool artifact_endpoint) {
        if (e.domain==ErrorDomain::authentication && !e.retryable) fail(4);
        if (stop_requested()) return e;
        // Legacy v1 validator predates the stable resource code set. Its local
        // owning-map mismatch is still a known metadata rejection, not transport uncertainty.
        const auto classification=e.domain==ErrorDomain::artifact && e.server_code=="coordinate_mode_mismatch"
            ? std::optional<ResourceFailure>{ResourceFailure::artifact_metadata_mismatch}
            : classify_resource_failure(e,artifact_endpoint);
        if (!classification) return e;
        const auto pending=state_->pending_status_events(); if (!pending) return pending.error();
        // No new failed after any unconfirmed status or publication-state ambiguity.
        if (std::any_of(pending.value().begin(),pending.value().end(),[&](const auto& event) {
            return event.generation.task_id==a.task_id;
        })) return e;
        const auto generation=state_->task_generation({a.task_id,a.attempt}); if (!generation) return generation.error();
        if (confirmed_or_retired(generation.value().state)) return e;
        return status(a,TaskExecutionState::failed,generation.value().progress,R"({"phase":"resource_rejected"})",resource_failure_code(*classification));
    }
    void work() {
        {
            std::unique_lock<std::mutex> guard(mutex_);
            condition_.wait(guard,[this] { return connection_.load()==ConnectionState::online || stop_requested() || stopping_; });
        }
        if (stop_requested()) return;
        if (!replay_pending()) { if (!stop_requested()) fail(5); return; }
        const auto active=state_->active_task_keys(); if (!active) { error_record(active.error()); fail(5); return; }
        for (const auto& key : active.value()) {
            if (stop_requested()) return;
            const auto task=http_->recover_accepted_task(key,*state_);
            if (!task) {
                if (task.error().server_code=="lease_expired") {
                    static_cast<void>(state_->mark_task_generation_expired(key)); record("active_expired",&key); continue;
                }
                error_record(task.error()); fail(5); return;
            }
            const auto lease=state_->active_task_lease(key); if (!lease) { fail(5); return; }
            record("active_recovery_no_accept",&key);
            const auto result=process(lease.value().notification,task.value());
            if (!result) { error_record(result.error()); if (!stop_requested()) fail(5); return; }
        }
        record("startup_recovery_complete");
        while (!stop_requested()) {
            TaskAvailableMessage n;
            {
                std::unique_lock<std::mutex> guard(mutex_);
                condition_.wait(guard,[this] { return !queue_.empty() || stop_requested() || stopping_; });
                if (stop_requested() || stopping_) return;
                n=queue_.front(); queue_.pop_front(); working_=n;
            }
            const auto result=process(n);
            device_->release_task_notification(n.task_id,n.attempt);
            {
                std::lock_guard<std::mutex> guard(mutex_); working_.reset();
            }
            if (!result) {
                error_record(result.error());
                if (result.error().server_code=="lease_expired") record("active_expired");
                if (!stop_requested()) fail(5);
                return;
            }
        }
    }
    Settings settings_;
    std::unique_ptr<DurableStateStore> state_;
    std::unique_ptr<DeviceTaskHttpClient> http_;
    std::unique_ptr<DeviceClient> device_;
    std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<TaskAvailableMessage> queue_;
    std::optional<TaskAvailableMessage> working_;
    std::thread worker_;
    CancellationSignal cancellation_;
    std::atomic<ConnectionState> connection_{ConnectionState::stopped};
    std::atomic<int> exit_code_{0};
    bool stopping_{false};
};
}
int main(int argc,char** argv) {
    try {
        std::filesystem::path config; bool check=false,run=false,allow_remote=false,confirm=false;
        std::string resolve;
        for (int i=1;i<argc;++i) {
            const std::string option=argv[i];
            if (option=="--help") {
                std::cout << "roc_vehicle_driver --config /private/config.json --precheck|--run [--allow-remote]\n"
                             "Manual local coordination: --config FILE --resolve EVENT_UUID --confirm-coordination\n"
                             "No CLI token, task polling, automatic resolve or TLS verification bypass.\n"; return 0;
            }
            if (option=="--config" && i+1<argc) config=argv[++i];
            else if (option=="--precheck") check=true;
            else if (option=="--run") run=true;
            else if (option=="--allow-remote") allow_remote=true;
            else if (option=="--resolve" && i+1<argc) resolve=argv[++i];
            else if (option=="--confirm-coordination") confirm=true;
            else { record("arguments_invalid"); return 2; }
        }
        if (config.empty() || static_cast<int>(check)+static_cast<int>(run)+static_cast<int>(!resolve.empty())!=1 ||
            (confirm != !resolve.empty()) || (!resolve.empty() && !is_valid_uuid(resolve))) { record("arguments_invalid"); return 2; }
        const auto settings=load_settings(config,allow_remote);
        if (!settings) { record(settings.error().server_code); return 2; }
        const auto valid=preflight(settings.value());
        if (!valid) { record(valid.error().server_code); return 2; }
        if (check) { record("precheck_ok_no_network_wire_rules"); return 0; }
        const auto locks=RootLocks::acquire(settings.value());
        if (!locks) { record(locks.error().server_code); return 3; }
        Json binding(jsoncons::json_object_arg);
        binding["vehicle_id"]=settings.value().client.expected_vehicle_id;
        binding["origin_sha256"]=sha256_hex(settings.value().client.base_url).value();
        binding["state_root_sha256"]=sha256_hex(settings.value().client.state_directory.string()).value();
        binding["artifact_root_sha256"]=sha256_hex(settings.value().client.artifact_directory.string()).value();
        binding["system_commit"]="a9c601d6977d89000438854a24e3199573004a2f";
        for (const auto& root : {settings.value().client.state_directory,settings.value().client.artifact_directory}) {
            const auto saved=save_immutable(root/"driver-binding.json",binding.to_string());
            if (!saved) { record("root_binding_conflict"); return 3; }
        }
        auto store=DurableStateStore::open(settings.value().client.state_directory/"roc-vehicle-state.sqlite3");
        if (!store) { error_record(store.error()); return 5; }
        if (!resolve.empty()) {
            const auto done=store.value()->resolve_status_event_reconciliation(resolve);
            if (!done) { error_record(done.error()); return 5; }
            record("manual_resolved_no_network"); return 0;
        }
        const auto path=settings.value().token_file;
        auto http=DeviceTaskHttpClient::create(settings.value().client,[path] { return read_token(path); });
        if (!http) { error_record(http.error()); return 5; }
        std::signal(SIGTERM,signal_stop); std::signal(SIGINT,signal_stop); std::signal(SIGPIPE,SIG_IGN);
        Coordinator coordinator(settings.value(),std::move(store.value()),std::move(http.value()));
        return coordinator.run();
    } catch (...) { record("exception_redacted"); return 6; }
}
