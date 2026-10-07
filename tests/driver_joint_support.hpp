#pragma once
#include "v08_wire_support.hpp"
#include "driver_support.hpp"
#include "contract_validation.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/beast/websocket.hpp>
#include <spawn.h>
#include <sys/wait.h>
#include <csignal>
#include <deque>
#include <map>
#include <mutex>
extern char** environ;

namespace driver_test {
using namespace v08_wire;
namespace websocket=beast::websocket;
inline constexpr auto driver_token="synthetic-driver-joint-token";
inline constexpr auto vehicle_id="123e4567-e89b-12d3-a456-426614174001";
inline void write_private(const std::filesystem::path& path,std::string_view text) {
    const int fd=::open(path.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
    CHECK(fd>=0); if (fd<0) return;
    CHECK(::write(fd,text.data(),text.size())==static_cast<ssize_t>(text.size()));
    CHECK(::fsync(fd)==0); CHECK(::close(fd)==0);
}
inline std::filesystem::path temporary_root() {
    std::array<char,40> pattern{};const std::string name="/tmp/roc-driver-joint-XXXXXX";
    std::copy(name.begin(),name.end(),pattern.begin()); const auto* made=::mkdtemp(pattern.data());
    CHECK(made); return made?std::filesystem::path{made}:std::filesystem::path{};
}
inline Json settings(const std::filesystem::path& root,std::uint16_t port) {
    Json j(jsoncons::json_object_arg);j["base_url"]="http://127.0.0.1:"+std::to_string(port);
    j["vehicle_id"]=vehicle_id;j["transport"]="http-ws-demo";j["token_file"]=(root/"device-token").string();
    j["state_directory"]=(root/"state").string();j["artifact_directory"]=(root/"artifacts").string();
    j["queue_capacity"]=8;j["retry_attempts"]=2;j["retry_initial_ms"]=20;j["retry_maximum_ms"]=40;
    j["http_operation_timeout_ms"]=200;j["hello_timeout_ms"]=500;j["offline_budget_ms"]=5000;
    j["reconnect_initial_ms"]=20;j["reconnect_maximum_ms"]=80;return j;
}
class Process {
public:
    Process(const std::filesystem::path& root,const std::filesystem::path& config,std::vector<std::string> arguments={"--run"},
            const char* executable=ROC_DRIVER_EXECUTABLE) {
        const auto id=generate_uuid_v4();CHECK(id);log=root/("driver-"+id.value()+".log");
        const int fd=::open(log.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0600);CHECK(fd>=0);
        std::vector<std::string> all{executable,"--config",config.string()};
        all.insert(all.end(),arguments.begin(),arguments.end());std::vector<char*> pointers;
        for (auto& a:all)pointers.push_back(a.data());
        pointers.push_back(nullptr);
        posix_spawn_file_actions_t actions;CHECK(::posix_spawn_file_actions_init(&actions)==0);
        CHECK(::posix_spawn_file_actions_adddup2(&actions,fd,STDOUT_FILENO)==0);
        CHECK(::posix_spawn_file_actions_adddup2(&actions,fd,STDERR_FILENO)==0);
        CHECK(::posix_spawn_file_actions_addclose(&actions,fd)==0);
        const int rc=::posix_spawn(&pid,executable,&actions,nullptr,pointers.data(),environ);
        CHECK(rc==0);if (rc!=0)pid=-1;
        CHECK(::posix_spawn_file_actions_destroy(&actions)==0);if (fd>=0)CHECK(::close(fd)==0);
    }
    ~Process() { if (pid>0) {static_cast<void>(::kill(pid,SIGKILL));static_cast<void>(::waitpid(pid,nullptr,0));} }
    int wait(int milliseconds=8000) {
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds{milliseconds};int status=0;
        while (pid>0 && std::chrono::steady_clock::now()<deadline) {
            const auto result=::waitpid(pid,&status,WNOHANG);
            if (result==pid) {pid=-1;return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);}
            CHECK(result==0);std::this_thread::sleep_for(std::chrono::milliseconds{5});
        }
        CHECK(false);return -1;
    }
    void signal(int value) {CHECK(pid>0 && ::kill(pid,value)==0);}
    bool contains(std::string_view text) const {return road_test::read(log).find(text)!=std::string::npos;}
    void redacted() const {
        const auto text=road_test::read(log);
        CHECK(text.find(driver_token)==std::string::npos);CHECK(text.find("synthetic-joint-lease")==std::string::npos);
        CHECK(text.find("PRIVATE KEY")==std::string::npos);CHECK(text.find("\"event_id\"")==std::string::npos);
        CHECK(text.find("AddressSanitizer")==std::string::npos);CHECK(text.find("runtime error:")==std::string::npos);
    }
    pid_t pid=-1;std::filesystem::path log;
};
template<class Predicate> bool eventually(Predicate predicate,int milliseconds=5000) {
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::milliseconds{milliseconds};
    while (std::chrono::steady_clock::now()<deadline) {
        if (predicate())return true;
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
    return predicate();
}
struct Task {
    Resource input;std::string id;std::string state="offered";int attempt=0,notice_attempt=0,progress=0;
    std::string active_lease, failure_code;std::int64_t expiry=4102444800;
    int accepts=0,downloads=0,manifests=0,delivered=0,failed=0;
    std::map<std::string,std::pair<std::string,std::string>> events;
    std::vector<std::string> terminal_bodies,terminal_leases;
    bool offered=true;
};
class JointFixture {
    struct HttpSession;struct WsSession;
public:
    explicit JointFixture(std::filesystem::path root,int kind,int count=1):root_(std::move(root)),acceptor_(io_,{asio::ip::address_v4::loopback(),0}) {
        for (int i=0;i<count;++i) {
            Task t;t.input=resource(kind);t.id=task;t.id.back()=static_cast<char>('0'+i);
            t.input.manifest.task_id=t.id;t.input.manifest.artifact_url=t.input.manifest.package_version?"":task_artifact_target(t.id);
            for (auto& file:t.input.manifest.files)file.artifact_url=task_artifact_file_target(t.id,file.id);
            tasks.push_back(std::move(t));
        }
        accept();thread_=std::thread([this] {io_.run();});
    }
    ~JointFixture() {stop();}
    std::uint16_t port()const{return acceptor_.local_endpoint().port();}
    template<class F> auto inspect(F f) {
        std::lock_guard<std::mutex> guard(mutex);return f(*this);
    }
    void notify(int index,int repetitions=1) {asio::post(io_,[this,index,repetitions] {notify_on_io(index,repetitions);});}
    void reoffer(int index) {
        {std::lock_guard<std::mutex> guard(mutex);auto& t=tasks[static_cast<std::size_t>(index)];t.offered=true;t.state="offered";t.notice_attempt=t.attempt;}
        notify(index,2);
    }
    void disconnect() {asio::post(io_,[this] {for (auto& weak:websockets_)if (auto s=weak.lock())s->close();});}
    void release_stalls() {
        asio::post(io_,[this] {for (auto& weak:http_sessions_)if (auto s=weak.lock())s->close_delayed();});
    }
    void stop() {
        if (!thread_.joinable())return;
        asio::post(io_,[this] {
            stopped_=true;boost::system::error_code ec;acceptor_.close(ec);
            for (auto& weak:websockets_)if (auto s=weak.lock())s->close();
            for (auto& weak:http_sessions_)if (auto s=weak.lock())s->close();
        });thread_.join();
    }
    std::mutex mutex;
    std::vector<Task> tasks;
    std::vector<std::string> requests;
    bool good=true,wrong_identity=false,hash_failure=false,drop_terminal=false,refuse_replay=false;
    int failure_reason=0;
    bool terminal_dropped=false,stall_download=false,download_stalled=false,notify_enabled=true,ignore_hello=false;
    std::string ws_error;
    int handshakes=0,heartbeats=0,telemetries=0;
private:
    void bad(int reason){good=false;failure_reason=reason;}
    std::string envelope(const char* type,Json payload) {
        Json j(jsoncons::json_object_arg);j["protocol_version"]=1;j["message_id"]=generate_uuid_v4().value();
        j["type"]=type;j["sequence"]=std::to_string(++server_sequence_);j["timestamp"]="2026-10-06T00:00:00Z";j["payload"]=std::move(payload);return j.to_string();
    }
    Json task_snapshot(const Task& t) {
        auto j=snapshot(t.state,t.attempt,t.progress);j["id"]=t.id;j["vehicle_id"]=vehicle_id;
        if(t.state=="failed")j["error_code"]=t.failure_code;
        return j;
    }
    std::string notice_body(const Task& t) {
        const auto& m=t.input.manifest;Json p(jsoncons::json_object_arg);p["task_id"]=t.id;
        p["batch_id"]=m.batch_id;p["project_id"]=m.project_id;p["resource_type"]=m.resource_type==ResourceType::map?"map":"road_network";
        p["resource_revision_id"]=m.resource_revision_id;p["attempt"]=t.notice_attempt;p["max_attempts"]=3;
        return envelope("task.available",std::move(p));
    }
    void notify_on_io(int index,int repetitions) {
        std::lock_guard<std::mutex> guard(mutex);
        for (int n=0;n<repetitions;++n) {
            const auto text=notice_body(tasks[static_cast<std::size_t>(index)]);
            for(auto& weak:websockets_)if(auto s=weak.lock())s->send(text);
        }
    }
    struct WsSession:std::enable_shared_from_this<WsSession> {
        JointFixture& owner;websocket::stream<beast::tcp_stream> socket;beast::flat_buffer buffer;
        std::deque<std::string> writes;bool closing=false;
        WsSession(JointFixture& f,beast::tcp_stream s):owner(f),socket(std::move(s)){}
        void start(http::request<http::string_body> req) {
            socket.async_accept(req,[self=shared_from_this()](boost::system::error_code ec) {
                if(ec)return;
                {
                    std::lock_guard<std::mutex> guard(self->owner.mutex);++self->owner.handshakes;
                    if(self->owner.ignore_hello){self->read();return;}
                    Json p=Json::parse(R"({"heartbeat_interval_seconds":1,"idle_timeout_seconds":5,"max_message_bytes":65536,"max_heartbeat_bytes":4096,"max_telemetry_bytes":16384,"last_client_sequence":"0"})");
                    p["vehicle_id"]=self->owner.wrong_identity?"123e4567-e89b-12d3-a456-426614174009":vehicle_id;
                    p["last_client_sequence"]=std::to_string(self->owner.cursor_);
                    self->send(self->owner.envelope("hello",p));
                    if (!self->owner.ws_error.empty()) {
                        Json e(jsoncons::json_object_arg);e["code"]=self->owner.ws_error;e["message"]=driver_token;
                        self->send(self->owner.envelope("error",e));
                    } else if(self->owner.notify_enabled) {
                        for(const auto& t:self->owner.tasks)if(t.offered)for(int i=0;i<2;++i)self->send(self->owner.notice_body(t));
                    }
                }
                self->read();
            });
        }
        void send(std::string text) {
            if (closing)return;
            const bool first=writes.empty();writes.push_back(std::move(text));if(first)write();
        }
        void write() {
            socket.text(true);socket.async_write(asio::buffer(writes.front()),[self=shared_from_this()](boost::system::error_code ec,std::size_t) {
                if(ec)return;
                self->writes.pop_front();if(!self->writes.empty())self->write();
            });
        }
        void read() {
            socket.async_read(buffer,[self=shared_from_this()](boost::system::error_code ec,std::size_t) {
                if(ec)return;
                const auto bytes=beast::buffers_to_string(self->buffer.data());self->buffer.consume(self->buffer.size());
                try {
                    if (!detail::valid_device_contract(bytes,"upstream")) throw std::invalid_argument("uplink");
                } catch (...) {std::lock_guard<std::mutex> guard(self->owner.mutex);self->owner.bad(1);return;}
                const auto j=Json::parse(bytes);{
                    std::lock_guard<std::mutex> guard(self->owner.mutex);
                    const auto seq=parse_positive_sequence(j["sequence"].as<std::string>());
                    if(!seq || seq.value()<=self->owner.cursor_)self->owner.bad(2);
                    else self->owner.cursor_=seq.value();
                    if(j["type"].as<std::string>()=="heartbeat")++self->owner.heartbeats;
                    if(j["type"].as<std::string>()=="telemetry")++self->owner.telemetries;
                    Json p(jsoncons::json_object_arg);p["ack_message_id"]=j["message_id"];p["ack_sequence"]=j["sequence"];
                    p["accepted_type"]=j["type"];p["duplicate"]=false;self->send(self->owner.envelope("ack",p));
                }self->read();
            });
        }
        void close() {closing=true;boost::system::error_code ec;socket.next_layer().socket().cancel(ec);socket.next_layer().socket().close(ec);}
    };
    struct HttpSession:std::enable_shared_from_this<HttpSession> {
        JointFixture& owner;beast::tcp_stream socket;beast::flat_buffer buffer;http::request<http::string_body> req;
        http::response<http::string_body> response;asio::steady_timer timer;bool delayed=false;
        HttpSession(JointFixture& f,tcp::socket s):owner(f),socket(std::move(s)),timer(owner.io_){}
        void start() {
            socket.expires_after(std::chrono::seconds{5});
            http::async_read(socket,buffer,req,[self=shared_from_this()](boost::system::error_code ec,std::size_t) {
                if(ec)return;
                {std::lock_guard<std::mutex> guard(self->owner.mutex);
                    if(self->req[http::field::authorization]!=std::string{"Device "}+driver_token)self->owner.bad(3);}
                if(websocket::is_upgrade(self->req)) {
                    {std::lock_guard<std::mutex> guard(self->owner.mutex);if(self->req.target()!="/ws/device")self->owner.bad(4);}
                    self->socket.expires_never();auto ws=std::make_shared<WsSession>(self->owner,std::move(self->socket));
                    self->owner.websockets_.push_back(ws);ws->start(std::move(self->req));return;
                }
                self->handle();
            });
        }
        void handle() {
            std::lock_guard<std::mutex> guard(owner.mutex);
            const auto target=std::string{req.target()};owner.requests.push_back(target);
            response.version(11);response.result(http::status::ok);response.keep_alive(false);response.set(http::field::content_type,"application/json");
            bool found=false,hold=false;
            for(auto& t:owner.tasks) {
                if(target.rfind("/api/device/tasks/"+t.id+"/",0)!=0)continue;
                found=true;
                const bool is_accept=target==task_accept_target(t.id);
                if(!req[http::field::range].empty() || (!is_accept && req["X-Task-Lease"].empty()))owner.bad(5);
                if(is_accept) {
                    if(req.method()!=http::verb::post || !req.body().empty() || !req["X-Task-Lease"].empty())owner.bad(6);
                    if(t.state!="offered" && t.state!="accepted") {reject("invalid_state",409);break;}
                    const bool replay=t.state=="accepted";if(!replay){++t.attempt;t.state="accepted";t.offered=false;t.active_lease="synthetic-joint-lease-"+std::to_string(t.attempt);}
                    ++t.accepts;Json j(jsoncons::json_object_arg);j["ok"]=true;j["replayed"]=replay;j["task"]=owner.task_snapshot(t);
                    j["lease_token"]=t.active_lease;j["lease_expires_at"]=t.expiry;response.body()=j.to_string();
                } else if(target==task_status_target(t.id)) {
                    if(req.method()!=http::verb::post || !detail::valid_task_contract(req.body(),"statusRequest")){owner.bad(7);reject("invalid_json",400);break;}
                    const auto j=Json::parse(req.body());const auto event=j["event_id"].as<std::string>();const auto state=j["state"].as<std::string>();
                    const std::string lease_header{req["X-Task-Lease"]};
                    if(state=="delivered"){t.terminal_bodies.push_back(req.body());t.terminal_leases.push_back(lease_header);}
                    const auto old=t.events.find(event);bool replay=old!=t.events.end();
                    if(replay) {
                        if(old->second.first!=req.body() || old->second.second!=lease_header){owner.bad(8);reject("event_conflict",409);break;}
                    } else {
                        if(state=="delivered" && owner.refuse_replay && owner.terminal_dropped){reject("invalid_lease",401);break;}
                        if(lease_header!=t.active_lease || j["progress"].as<int>()<t.progress){owner.bad(9);reject("invalid_lease",401);break;}
                        const bool transition=(state=="downloading" && t.state=="accepted") ||
                            (state=="delivering" && t.state=="downloading") || (state=="delivered" && t.state=="delivering") ||
                            (state=="failed" && (t.state=="downloading" || t.state=="delivering"));
                        if(!transition){owner.bad(10);reject("invalid_transition",409);break;}
                        if(state=="delivered") {
                            // The program's complete request must already be in its real SQLite Outbox.
                            auto db=DurableStateStore::open(owner.root_/"state"/"roc-vehicle-state.sqlite3");
                            if(!db)owner.bad(11);else {const auto saved=db.value()->status_event(event);
                                if(!saved)owner.bad(1200+static_cast<int>(saved.error().code));
                                else if(saved.value().payload_json!=req.body())owner.bad(121);
                                else if(saved.value().lease_token!=lease_header)owner.bad(122);
                                else if(saved.value().local_delivery_evidence.empty())owner.bad(123);}
                            const auto root=owner.root_/"artifacts"/("task-"+t.id+"-"+std::to_string(t.attempt));
                            if(t.input.manifest.package_version==2){if(!inspect_map_package(root,t.input.manifest))owner.bad(13);}
                            else if(!inspect_file_artifact(root,t.input.manifest))owner.bad(14);
                        }
                        if(!(state=="delivered" && owner.drop_terminal && owner.refuse_replay && !owner.terminal_dropped)) {
                            t.events[event]={req.body(),lease_header};t.state=state;t.progress=j["progress"].as<int>();
                            if(state=="delivered")++t.delivered;
                            if(state=="failed"){++t.failed;t.failure_code=j["error_code"].as<std::string>();}
                        }
                    }
                    if(state=="delivered" && owner.drop_terminal && !owner.terminal_dropped){owner.terminal_dropped=true;hold=true;}
                    Json body(jsoncons::json_object_arg);body["ok"]=true;body["replayed"]=replay;
                    body["project_id"]=t.input.manifest.project_id;body["task"]=owner.task_snapshot(t);response.body()=body.to_string();
                } else {
                    if(req.method()!=http::verb::get || req["X-Task-Lease"]!=t.active_lease){owner.bad(15);reject("invalid_lease",401);break;}
                    if(target==task_manifest_target(t.id)){++t.manifests;response.body()=driver::manifest_document(t.input.manifest);}
                    else {
                        ++t.downloads;bool file_found=false;
                        if(t.input.manifest.package_version==2)for(const auto& file:t.input.manifest.files)if(target==file.artifact_url) {
                            response.set(http::field::content_type,file.content_type);response.set("X-Content-SHA256",file.sha256);
                            response.body()=t.input.package.bytes.at(file.id);file_found=true;
                        }
                        if(t.input.manifest.package_version==0 && target==task_artifact_target(t.id)) {
                            response.set(http::field::content_type,t.input.manifest.content_type);response.set("X-Content-SHA256",t.input.manifest.sha256);
                            response.body()=t.input.bytes;file_found=true;
                        }
                        if(!file_found){owner.bad(16);reject("artifact_not_found",404);}
                        if(owner.hash_failure && !response.body().empty())response.body()[0]='X';
                        if(owner.stall_download && !owner.download_stalled){owner.download_stalled=true;hold=true;}
                    }
                }
                break;
            }
            if(!found){owner.bad(17);reject("task_not_found",404);}
            response.prepare_payload();
            if(hold) {
                delayed=true;timer.expires_after(std::chrono::seconds{5});timer.async_wait([self=shared_from_this()](boost::system::error_code ec){if(!ec)self->close();});
            } else send();
        }
        void reject(const char* code,unsigned status) {
            response.result(static_cast<http::status>(status));response.set(http::field::content_type,"application/json");
            response.body()=std::string{R"({"ok":false,"code":")"}+code+R"(","message":"synthetic-driver-joint-token"})";
        }
        void send() {http::async_write(socket,response,[self=shared_from_this()](boost::system::error_code,std::size_t){self->close();});}
        void close_delayed() {if(delayed)close();}
        void close() {boost::system::error_code ec;static_cast<void>(timer.cancel());socket.socket().cancel(ec);socket.socket().close(ec);}
    };
    void accept() {
        acceptor_.async_accept([this](boost::system::error_code ec,tcp::socket s) {
            if(!ec){auto session=std::make_shared<HttpSession>(*this,std::move(s));http_sessions_.push_back(session);session->start();}
            if(!stopped_)accept();
        });
    }
    std::filesystem::path root_;asio::io_context io_;tcp::acceptor acceptor_;std::thread thread_;
    std::vector<std::weak_ptr<WsSession>> websockets_;std::vector<std::weak_ptr<HttpSession>> http_sessions_;
    std::int64_t cursor_=0,server_sequence_=0;bool stopped_=false;
};
}
