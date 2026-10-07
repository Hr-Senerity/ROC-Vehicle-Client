#pragma once
#include "road_test_support.hpp"
#include "package_test_support.hpp"
#include "roc/vehicle/task_http_client.hpp"
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

namespace v08_wire {
using namespace roc::vehicle;
namespace asio=boost::asio; namespace beast=boost::beast;
namespace http=beast::http; namespace ssl=asio::ssl;
using tcp=asio::ip::tcp; using Json=road_test::Json;
inline int failures=0;
inline void check(bool ok,const char* text,int line) {
    if (!ok) { std::cerr<<line<<": "<<text<<'\n';++failures; }
}
#define CHECK(value) ::v08_wire::check(static_cast<bool>(value),#value,__LINE__)
inline constexpr auto task=road_test::task_id;
inline constexpr auto lease="synthetic-v08-wire-lease";
inline constexpr auto token="synthetic-v08-wire-token";
inline constexpr auto event_id="123e4567-e89b-42d3-a456-426614174050";
inline Json snapshot(std::string state,int attempt=1,int progress=100) {
    auto j=Json::parse(R"({"id":"123e4567-e89b-12d3-a456-426614174010",
      "batch_id":"123e4567-e89b-12d3-a456-426614174011",
      "vehicle_id":"123e4567-e89b-12d3-a456-426614174001","state":"delivered",
      "attempt":1,"max_attempts":3,"progress":100,"error_code":null,"error_message":null,
      "offered_at":null,"accepted_at":null,"delivered_at":null,"updated_at":"2026-10-05T00:00:00Z"})");
    j["state"]=state;j["attempt"]=attempt;j["progress"]=progress;return j;
}
inline std::string status_response(bool replay=true,int attempt=2) {
    Json j(jsoncons::json_object_arg);j["ok"]=true;j["replayed"]=replay;
    j["project_id"]="123e4567-e89b-12d3-a456-426614174012";
    // Historical exact replay can return a newer, nonterminal task snapshot.
    j["task"]=snapshot(attempt==2?"downloading":"delivered",attempt,100);return j.to_string();
}
inline ClientConfig config(const std::filesystem::path& root,std::uint16_t port,bool tls=false) {
    ClientConfig c;c.base_url=std::string{tls?"https://127.0.0.1:":"http://127.0.0.1:"}+std::to_string(port);
    c.state_directory=root/"state";c.artifact_directory=root/"artifacts";
    c.artifact_limits.stream_chunk_bytes=3;c.http_operation_timeout=std::chrono::milliseconds{200};return c;
}
inline auto client(const ClientConfig& c) {
    return DeviceTaskHttpClient::create(c,[]{return Result<std::string>{token};});
}
inline TaskAvailableMessage notice(const ArtifactManifest& m) {
    TaskAvailableMessage n;n.task_id=task;n.batch_id=m.batch_id;n.project_id=m.project_id;
    n.resource_revision_id=m.resource_revision_id;n.resource_type=m.resource_type;n.max_attempts=3;return n;
}
inline AcceptedTask seed(DurableStateStore& store,const ArtifactManifest& m,bool terminal=false,
                         std::int64_t expiry=4102444800) {
    CHECK(store.register_task_generation({task,1},30));
    CHECK(store.persist_active_task_lease({notice(m),{task,1},lease,expiry,30}));
    AcceptedTask a;a.task_id=task;a.attempt=1;a.max_attempts=3;a.batch_id=m.batch_id;
    a.lease_token=lease;a.lease_expires_at=expiry;a.progress_floor=30;
    const int count=terminal?2:1;
    for (int i=0;i<count;++i) {
        StatusEventRecord e;e.event_id=i==0?"123e4567-e89b-42d3-a456-426614174051":"123e4567-e89b-42d3-a456-426614174052";
        e.generation={task,1};e.state=i==0?TaskExecutionState::downloading:TaskExecutionState::delivering;
        e.progress=i==0?30:75;e.lease_token=lease;e.lease_expires_at=a.lease_expires_at;
        e.local_delivery_evidence=R"({"phase":"synthetic_preparation"})";e.payload_json=make_status_request(e).value();
        CHECK(store.persist_status_event(e));CHECK(store.acknowledge_status_event(e.event_id));
        CHECK(store.active_task_lease({task,1}));
    }
    return a;
}
struct Resource { std::string name;ArtifactManifest manifest;std::string bytes;package_test::Fixture package; };
inline Resource resource(int kind) {
    Resource r;
    if (kind>=2 && kind<=4) {
        r.name=kind==2?"package-png":kind==3?"package-jpeg":"package-pgm-yaml";
        r.package=package_test::fixture(kind==2?"png":kind==3?"jpeg":"pgm-yaml");
        r.manifest=r.package.manifest;r.bytes=r.package.bytes.at(r.manifest.files.back().id);
    } else if (kind>=5) {
        r.name=kind==5?"road-v1":"road-v2";
        r.bytes=kind==5?R"({"schema_version":1,"coordinate_mode":"metric","nodes":[],"edges":[]})":road_test::fixture().to_string();
        r.manifest=road_test::manifest(r.bytes,kind==5?1:2);
    } else {
        r.name=kind==0?"legacy-png":"legacy-jpeg";r.bytes="synthetic original image bytes";
        r.manifest=road_test::manifest(r.bytes);r.manifest.schema_version=0;
        r.manifest.resource_type=ResourceType::map;r.manifest.content_type=kind==0?"image/png":"image/jpeg";
    }
    return r;
}
// Each server is bounded and stoppable even if the client refuses before TCP.
// Start only after fork: never fork a process that has this thread running.
class Server {
public:
    Server():acceptor_(context_,{asio::ip::address_v4::loopback(),0}) { acceptor_.non_blocking(true); }
    std::uint16_t port() const {return acceptor_.local_endpoint().port();}
    void start(std::function<void(tcp::socket&)> handler,int count=1) {
        thread_=std::thread([this,handler=std::move(handler),count] {
            for (int i=0;i<count && !stop_.load();++i) {
                boost::system::error_code ec;tcp::socket socket(context_);
                while (!stop_.load()) {
                    acceptor_.accept(socket,ec);
                    if (!ec) break;
                    if (ec!=asio::error::would_block && ec!=asio::error::try_again) {good=false;return;}
                    std::this_thread::sleep_for(std::chrono::milliseconds{2});
                }
                if (stop_.load()) return;
                ++connections;handler(socket);
            }
        });
    }
    void finish() {stop_.store(true);if (thread_.joinable())thread_.join();}
    ~Server(){finish();}
    bool good=true;int connections=0;std::vector<std::string> bodies,leases,targets;
private:asio::io_context context_;tcp::acceptor acceptor_;std::atomic<bool> stop_{false};std::thread thread_;
};
template<typename Stream>
bool request(Stream& stream,Server& server,std::string_view target,http::verb verb=http::verb::get) {
    boost::system::error_code ec;beast::flat_buffer b;http::request<http::string_body> r;
    http::read(stream,b,r,ec);if (ec){server.good=false;return false;}
    server.targets.emplace_back(r.target());server.bodies.push_back(r.body());server.leases.emplace_back(r["X-Task-Lease"]);
    if (r.target()!=target || r.method()!=verb || r[http::field::authorization]!=std::string{"Device "}+token ||
        (verb==http::verb::get && r["X-Task-Lease"]!=lease) || !r[http::field::range].empty()) server.good=false;
    return true;
}
template<typename Stream> void json(Stream& stream,std::string body,unsigned status=200) {
    http::response<http::string_body> r{static_cast<http::status>(status),11};r.keep_alive(false);
    r.set(http::field::content_type,"application/json");r.body()=std::move(body);r.prepare_payload();
    boost::system::error_code ec;http::write(stream,r,ec);
}
struct Certificate {std::string cert,key;};
inline Certificate certificate(bool hostname=true,bool expired=false) {
    using Key=std::unique_ptr<EVP_PKEY,decltype(&EVP_PKEY_free)>;
    using Cert=std::unique_ptr<X509,decltype(&X509_free)>;
    using Ctx=std::unique_ptr<EVP_PKEY_CTX,decltype(&EVP_PKEY_CTX_free)>;
    Ctx ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA,nullptr),EVP_PKEY_CTX_free);CHECK(ctx);
    CHECK(EVP_PKEY_keygen_init(ctx.get())>0);CHECK(EVP_PKEY_CTX_set_rsa_keygen_bits(ctx.get(),2048)>0);
    EVP_PKEY* raw=nullptr;CHECK(EVP_PKEY_keygen(ctx.get(),&raw)>0);Key key(raw,EVP_PKEY_free);
    Cert cert(X509_new(),X509_free);CHECK(X509_set_version(cert.get(),2)==1);
    CHECK(ASN1_INTEGER_set(X509_get_serialNumber(cert.get()),1)==1);
    CHECK(X509_gmtime_adj(X509_get_notBefore(cert.get()),-7200));
    CHECK(X509_gmtime_adj(X509_get_notAfter(cert.get()),expired?-3600:86400));
    CHECK(X509_set_pubkey(cert.get(),key.get())==1);
    auto* name=X509_get_subject_name(cert.get());const unsigned char cn[]="Synthetic V08 loopback";
    CHECK(X509_NAME_add_entry_by_txt(name,"CN",MBSTRING_ASC,cn,-1,-1,0)==1);CHECK(X509_set_issuer_name(cert.get(),name)==1);
    X509V3_CTX extension_ctx;X509V3_set_ctx(&extension_ctx,cert.get(),cert.get(),nullptr,nullptr,0);
    auto* ext=X509V3_EXT_conf_nid(nullptr,&extension_ctx,NID_subject_alt_name,
        hostname?"IP:127.0.0.1":"DNS:wrong.invalid");CHECK(ext);
    CHECK(X509_add_ext(cert.get(),ext,-1)==1);X509_EXTENSION_free(ext);
    CHECK(X509_sign(cert.get(),key.get(),EVP_sha256())>0);
    const auto pem=[](auto write){BIO* bio=BIO_new(BIO_s_mem());CHECK(bio);CHECK(write(bio)==1);
        char* data=nullptr;const auto size=BIO_get_mem_data(bio,&data);
        std::string result(data,static_cast<std::size_t>(size));BIO_free(bio);return result;};
    return {pem([&](BIO* b){return PEM_write_bio_X509(b,cert.get());}),
        pem([&](BIO* b){return PEM_write_bio_PrivateKey(b,key.get(),nullptr,nullptr,0,nullptr,nullptr);})};
}
inline void save_ca(const std::filesystem::path& path,const Certificate& c) {
    const int fd=::open(path.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);CHECK(fd>=0);
    if(fd>=0){CHECK(::write(fd,c.cert.data(),c.cert.size())==static_cast<ssize_t>(c.cert.size()));CHECK(::close(fd)==0);}
}
} // namespace v08_wire
