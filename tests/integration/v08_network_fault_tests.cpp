#include "../v08_wire_support.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include "roc/vehicle/package_receiver.hpp"
#include "roc/vehicle/road_receiver.hpp"
#include <array>
using namespace v08_wire;
namespace {
enum class Fault {control,cut,short_body,reset,timeout,untrusted,hostname,expired,handshake,tls_cut,length_high,length_low,length_budget};
const char* name(Fault f) {
    switch(f) {
    case Fault::control:return "control";case Fault::cut:return "http-cut";
    case Fault::short_body:return "short-body";case Fault::reset:return "reset";
    case Fault::timeout:return "timeout";case Fault::untrusted:return "untrusted";
    case Fault::hostname:return "hostname";case Fault::expired:return "expired";
    case Fault::handshake:return "handshake";case Fault::tls_cut:return "tls-cut";
    case Fault::length_high:return "length-header-high";case Fault::length_low:return "length-header-low";
    case Fault::length_budget:return "length-header-over-budget";
    }return "invalid";
}
template<typename Stream> void artifact(Stream& stream,const std::string& bytes,
    const std::string& mime,const std::string& hash,Fault fault) {
    const bool partial=fault==Fault::cut || fault==Fault::reset || fault==Fault::timeout || fault==Fault::tls_cut;
    const auto body=(partial || fault==Fault::short_body)?bytes.substr(0,2):bytes;
    const auto length=fault==Fault::length_high?bytes.size()+1U:fault==Fault::length_low?bytes.size()-1U:
        fault==Fault::length_budget?64U*1024U*1024U:partial?bytes.size():body.size();
    const auto header="HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Type: "+mime+
        "\r\nX-Content-SHA256: "+hash+"\r\nContent-Length: "+std::to_string(length)+"\r\n\r\n";
    boost::system::error_code ec;asio::write(stream,asio::buffer(header+body),ec);
    if(fault==Fault::timeout) std::this_thread::sleep_for(std::chrono::milliseconds{450});
}
void flow(const std::filesystem::path& root,int kind,Fault fault,bool tls,
          const Certificate& valid,const Certificate& wrong,const Certificate& expired) {
    const auto resource_value=resource(kind);const auto& m=resource_value.manifest;
    Server server;auto c=config(root,server.port(),tls);
    auto store=DurableStateStore::open(c.state_directory/"state.sqlite3");CHECK(store);if(!store)return;
    const auto accepted=seed(*store.value(),m);
    const bool handshake_rejection=fault==Fault::untrusted || fault==Fault::hostname || fault==Fault::expired || fault==Fault::handshake;
    const auto& cert=fault==Fault::hostname?wrong:fault==Fault::expired?expired:valid;
    ssl::context ssl_context(ssl::context::tls_server);
    if(tls) {
        ssl_context.use_certificate_chain(asio::buffer(cert.cert));
        ssl_context.use_private_key(asio::buffer(cert.key),ssl::context::pem);
        if(fault!=Fault::untrusted){save_ca(c.state_directory/"synthetic-ca.pem",cert);c.tls.ca_file=c.state_directory/"synthetic-ca.pem";}
    }
    const int count=handshake_rejection?1:static_cast<int>(m.package_version?m.files.size():1U);
    server.start([&](tcp::socket& socket){
        if(fault==Fault::handshake){boost::system::error_code ec;asio::write(socket,asio::buffer(std::string{"not TLS"}),ec);return;}
        const auto exchange=[&](auto& stream){
            const auto index=server.targets.size();
            const auto target=m.package_version?m.files[index].artifact_url:m.artifact_url;
            if(!request(stream,server,target))return;
            const auto& bytes=m.package_version?resource_value.package.bytes.at(m.files[index].id):resource_value.bytes;
            const auto& mime=m.package_version?m.files[index].content_type:m.content_type;
            const auto& hash=m.package_version?m.files[index].sha256:m.sha256;
            artifact(stream,bytes,mime,hash,index+1U==static_cast<std::size_t>(count)?fault:Fault::control);
        };
        boost::system::error_code ec;
        if(tls){ssl::stream<tcp::socket&> stream(socket,ssl_context);stream.handshake(ssl::stream_base::server,ec);
            if(!ec){exchange(stream);if(fault==Fault::control)stream.shutdown(ec);}}
        else{exchange(socket);if(fault==Fault::reset)socket.set_option(asio::socket_base::linger(true,0),ec);}
    },count);
    auto http_client=client(c);CHECK(http_client);if(!http_client){server.finish();return;}
    std::optional<Error> failure;bool receipt=false;std::size_t streamed=0;
    const auto started=std::chrono::steady_clock::now();
    if(m.package_version) {
        auto sink=PackageSink::create(c.artifact_directory,m);CHECK(sink);if(!sink){server.finish();return;}
        for(const auto& file:m.files){
            auto downloaded=http_client.value()->download_artifact_file(m,file.id,accepted,*store.value(),
                [&](const std::uint8_t* b,std::size_t n){streamed+=n;return sink.value()->write(file.id,b,n);});
            if(!downloaded){failure=downloaded.error();break;}
            auto sealed=sink.value()->seal_file(file.id,downloaded.value());if(!sealed){failure=sealed.error();break;}
        }
        if(!failure){CHECK(sink.value()->seal());auto published=sink.value()->commit();CHECK(published);receipt=static_cast<bool>(published);
            if(published)CHECK(verify_map_package(published.value()));}
        else {CHECK(!sink.value()->commit());CHECK(package_test::published_count(c.artifact_directory)==0);}
    }else {
        auto sink=FileSink::create(c.artifact_directory,m);CHECK(sink);if(!sink){server.finish();return;}
        auto downloaded=http_client.value()->download_artifact(m,accepted,*store.value(),
            [&](const std::uint8_t* b,std::size_t n){streamed+=n;return sink.value()->write(b,n);});
        if(!downloaded)failure=downloaded.error();
        else {auto sealed=sink.value()->seal(downloaded.value());if(!sealed)failure=sealed.error();}
        if(!failure){auto published=sink.value()->commit();CHECK(published);receipt=static_cast<bool>(published);
            if(published)CHECK(inspect_file_artifact(c.artifact_directory,m));}
        else {CHECK(!sink.value()->commit());CHECK(!inspect_file_artifact(c.artifact_directory,m));}
    }
    const auto elapsed=std::chrono::steady_clock::now()-started;
    server.finish();CHECK(server.good);CHECK(server.connections==count);
    CHECK(server.targets.size()==(handshake_rejection?0U:static_cast<std::size_t>(count)));
    CHECK(!store.value()->has_unconfirmed_terminal_event(task).value());
    CHECK(store.value()->pending_status_events().value().empty());CHECK(store.value()->active_task_lease({task,1}));
    CHECK(store.value()->task_generation({task,1}).value().state==TaskExecutionState::downloading);
    if(fault==Fault::control){CHECK(!failure);CHECK(receipt);}
    else {
        CHECK(failure);CHECK(!receipt);
        if(failure) {
            if(handshake_rejection){CHECK(failure->domain==ErrorDomain::tls);CHECK(!classify_resource_failure(*failure,true));}
            else if(fault==Fault::short_body || fault==Fault::length_high || fault==Fault::length_low || fault==Fault::length_budget){
                CHECK(failure->server_code=="artifact_metadata_mismatch");CHECK(!failure->retryable);
                if(fault!=Fault::short_body){
                    const auto previous=m.package_version==2 && m.files.size()==2?resource_value.package.bytes.at(m.files[0].id).size():0U;
                    CHECK(streamed==previous); // offending response never reaches the sink
                }
            }
            else{CHECK(failure->domain==ErrorDomain::transport);CHECK(!classify_resource_failure(*failure,true));}
            if(fault==Fault::timeout){CHECK(failure->code==ErrorCode::timeout);CHECK(elapsed<std::chrono::seconds{2});}
        }
    }
    std::cout<<"G1 "<<resource_value.name<<' '<<(tls?"TLS ":"HTTP ")<<name(fault)
        <<" state=downloading lease=active requests="<<server.targets.size()
        <<" receipt="<<receipt<<" terminal=none\n";
}
}
int main(){
    const auto root=std::filesystem::temp_directory_path()/"roc-v08-network-faults";
    std::error_code ec;std::filesystem::remove_all(root,ec);
    const auto valid=certificate(),wrong=certificate(false),expired=certificate(true,true);
    for(int kind=0;kind<7;++kind){
        for(const auto fault:{Fault::control,Fault::cut,Fault::short_body,Fault::reset,Fault::timeout,Fault::length_high,Fault::length_low,Fault::length_budget})
            flow(root/(std::to_string(kind)+"-http-"+name(fault)),kind,fault,false,valid,wrong,expired);
        for(const auto fault:{Fault::control,Fault::untrusted,Fault::hostname,Fault::expired,Fault::handshake,Fault::tls_cut,Fault::length_high,Fault::length_low,Fault::length_budget})
            flow(root/(std::to_string(kind)+"-tls-"+name(fault)),kind,fault,true,valid,wrong,expired);
    }
    std::filesystem::remove_all(root,ec);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
