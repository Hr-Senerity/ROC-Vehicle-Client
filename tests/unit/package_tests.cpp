#include "../package_test_support.hpp"
#include "roc/vehicle/package_receiver.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include <jsoncons/json.hpp>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <sys/stat.h>

using namespace roc::vehicle;
using namespace package_test;
namespace {
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n'; ++failures; } } while(false)
void expect_mode(const std::filesystem::path& path,mode_t mode) {
    struct stat data {}; EXPECT(::lstat(path.c_str(),&data)==0); EXPECT((data.st_mode&07777)==mode);
}
void successful_package(const std::filesystem::path& root,std::string format,bool worst=false) {
    auto value=fixture(format);
    if (worst) {
        std::string name,mime="application/"+std::string(84,'x');
        for (int i=0;i<255;++i) name+="\xF0\x9F\x9A\x97";
        for (auto& file:value.manifest.files) {
            file.file_name=name;
            if (file.role!="image") file.content_type=mime;
        }
        value.manifest.content_type=mime; value.manifest.version=std::numeric_limits<std::int64_t>::max();
        value.manifest.image_width=std::numeric_limits<std::int64_t>::max();
        value.manifest.image_height=std::numeric_limits<std::int64_t>::max();
        value.manifest.resolution=std::numeric_limits<double>::max();
        value.manifest.origin_x=-std::numeric_limits<double>::max();
        value.manifest.origin_y=std::numeric_limits<double>::max();
        value.manifest.origin_theta=std::numeric_limits<double>::max();
    }
    auto sink=prepared(root,value); EXPECT(sink); if (!sink) return;
    EXPECT(published_count(root)==0); expect_mode(root,0700); expect_mode(sink.value()->staging_path(),0700);
    auto received=sink.value()->commit(); EXPECT(received); if (!received) return;
    EXPECT(published_count(root)==1); EXPECT(!std::filesystem::exists(sink.value()->staging_path()));
    const auto& package=received.value();
    for (const auto& file:value.manifest.files) {
        EXPECT(read(package.directory/(file.role+".bin"))==value.bytes.at(file.id));
        expect_mode(package.directory/(file.role+".bin"),0600);
    }
    expect_mode(package.directory/"manifest.json",0600);
    const auto descriptor_json=jsoncons::json::parse(read(package.directory/"manifest.json"));
    EXPECT(descriptor_json["manifest"]["origin"]["x"].as<double>()==*value.manifest.origin_x);
    EXPECT(descriptor_json["manifest"]["resolution"].as<double>()==*value.manifest.resolution);
    const auto evidence=make_map_package_evidence(package,
        {task_id,worst?std::numeric_limits<std::int64_t>::max():10}); EXPECT(evidence);
    if (evidence) {
        EXPECT(evidence.value().size()<2048); EXPECT(evidence.value().size()<=16384);
        EXPECT(evidence.value().find("lease")==std::string::npos);
        EXPECT(evidence.value().find("file_name")==std::string::npos);
        if (worst) {
            // Width bound, not a claim to have downloaded INT64_MAX-byte files.
            auto bound=jsoncons::json::parse(evidence.value());
            for (auto& file:bound["files"].array_range())
                file["byte_size"]=std::numeric_limits<std::int64_t>::max();
            EXPECT(bound.to_string().size()<2048);
        }
    }
    auto recovered=inspect_map_package(root,value.manifest); EXPECT(recovered);
    auto duplicate=prepared(root,value); EXPECT(duplicate);
    if (duplicate) {
        auto again=duplicate.value()->commit(); EXPECT(again);
        if (again) EXPECT(again.value().directory==package.directory);
        EXPECT(published_count(root)==1);
    }
    if (format=="pgm-yaml" && !worst) {
        auto reversed=value; std::reverse(reversed.manifest.files.begin(),reversed.manifest.files.end());
        auto reordered=inspect_map_package(root,reversed.manifest); EXPECT(reordered);
        if (reordered) EXPECT(reordered.value().directory==package.directory);
        auto modified=value.manifest; modified.origin_x=9.0;
        EXPECT(!inspect_map_package(root,modified)); // identical file digest is not identical metadata
    }
}
void absent_metadata(const std::filesystem::path& root) {
    auto value=fixture("png"); auto& manifest=value.manifest;
    manifest.coordinate_mode="legacy-normalized";
    manifest.resolution.reset(); manifest.origin_x.reset(); manifest.origin_y.reset();
    manifest.origin_theta.reset(); manifest.image_width.reset(); manifest.image_height.reset();
    auto sink=prepared(root,value); EXPECT(sink); if (!sink) return;
    auto package=sink.value()->commit(); EXPECT(package); if (!package) return;
    const auto stored=jsoncons::json::parse(read(package.value().directory/"manifest.json"))["manifest"];
    EXPECT(stored["resolution"].is_null()); EXPECT(stored["origin"]["x"].is_null());
    EXPECT(stored["origin"]["y"].is_null()); EXPECT(stored["origin"]["theta"].is_null());
    EXPECT(stored["image_width"].is_null() && stored["image_height"].is_null());
    EXPECT(!package.value().manifest.resolution && !package.value().manifest.origin_x);
}
void invalid_packages(const std::filesystem::path& root) {
    const auto value=fixture();
    auto changed=value.manifest; changed.package_version=1;
    auto result=PackageSink::create(root,changed); EXPECT(!result);
    EXPECT(result.error().server_code=="unsupported_package_version");
    changed=value.manifest; changed.files.clear(); result=PackageSink::create(root,changed);
    EXPECT(!result); EXPECT(result.error().server_code=="artifact_file_missing");
    changed=value.manifest; changed.sha256=std::string(64,'a'); result=PackageSink::create(root,changed);
    EXPECT(!result); EXPECT(result.error().server_code=="artifact_hash_mismatch");
    changed=value.manifest; changed.files[1].id=changed.files[0].id;
    EXPECT(!PackageSink::create(root,changed));
    changed=value.manifest; changed.files[1].role="pgm"; EXPECT(!PackageSink::create(root,changed));
    changed=value.manifest; changed.files[0].artifact_url="/escape"; EXPECT(!PackageSink::create(root,changed));
    ArtifactLimits limits; limits.maximum_map_bytes=1;
    EXPECT(!PackageSink::create(root,value.manifest,limits));
    EXPECT(!std::filesystem::exists(root));
}
void missing_and_bad_files(const std::filesystem::path& root) {
    auto value=fixture(); auto sink=PackageSink::create(root,value.manifest); EXPECT(sink); if (!sink) return;
    EXPECT(write_and_seal(*sink.value(),value,value.manifest.files.front()));
    auto sealed=sink.value()->seal(); EXPECT(!sealed); EXPECT(sealed.error().server_code=="artifact_file_missing");
    EXPECT(!sink.value()->commit()); EXPECT(published_count(root)==0);
    const auto staging=sink.value()->staging_path(); sink.value().reset(); EXPECT(!std::filesystem::exists(staging));
    for (int mode=0;mode<4;++mode) {
        auto bad=PackageSink::create(root,value.manifest); EXPECT(bad); if (!bad) continue;
        const auto& first=value.manifest.files[0]; const auto& second=value.manifest.files[1];
        EXPECT(write_and_seal(*bad.value(),value,first));
        auto bytes=value.bytes.at(second.id); if (mode==0) bytes[0]='X';
        EXPECT(bad.value()->write(second.id,reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
        auto headers=metadata(second);
        if (mode==1) headers.content_type="text/plain";
        if (mode==2) headers.content_length=second.byte_size+1;
        if (mode==3) headers.sha256=std::string(64,'a');
        auto failure=bad.value()->seal_file(second.id,headers); EXPECT(!failure);
        EXPECT(failure.error().server_code==(mode==0 || mode==3 ? "artifact_hash_mismatch":"artifact_metadata_mismatch"));
        EXPECT(!bad.value()->commit()); EXPECT(published_count(root)==0);
    }
}
void permission_and_recovery(const std::filesystem::path& root) {
    std::filesystem::create_directories(root); EXPECT(::chmod(root.c_str(),0755)==0);
    auto value=fixture(); EXPECT(!PackageSink::create(root,value.manifest)); expect_mode(root,0755);
    EXPECT(!PackageSink::create("/tmp",value.manifest)); EXPECT(!PackageSink::create("/",value.manifest));
    auto sink=PackageSink::create(root/"private",value.manifest); EXPECT(sink); if (!sink) return;
    expect_mode(root,0755); expect_mode(root/"private",0700);
    const auto outside=root/"keep.txt"; { std::ofstream file(outside); file<<"keep"; }
    const auto dangerous=root/"private"/".roc-package-123e4567-e89b-12d3-a456-426614174010-123e4567-e89b-12d3-a456-426614174019.part";
    std::filesystem::create_directory_symlink(root,dangerous);
    EXPECT(!recover_package_staging(root/"private")); EXPECT(read(outside)=="keep");
    std::filesystem::remove(dangerous); // test-owned symlink only; no recursive follow
    sink.value().reset();
    EXPECT(recover_package_staging(root/"private").value()==0);
    EXPECT(read(outside)=="keep"); expect_mode(root,0755);
}
struct ReceiverState {
    std::map<std::string,std::string> bytes;
    bool committed=false,aborted=false; int mode=0;
    CancellationSignal* cancel=nullptr; std::filesystem::path mutation;
};
class Transaction final : public PackageStreamTransaction {
public:
    explicit Transaction(ReceiverState& state) : state_(state) {}
    Result<void> write(std::string_view id,const std::uint8_t* bytes,std::size_t size) override {
        if (state_.mode==1) return make_resource_failure(ResourceFailure::receiver_commit_failed);
        if (state_.mode==2) throw std::runtime_error("synthetic private callback detail");
        state_.bytes[std::string{id}].append(reinterpret_cast<const char*>(bytes),size);
        if (state_.cancel) state_.cancel->cancel();
        if (!state_.mutation.empty()) { std::ofstream output(state_.mutation,std::ios::binary); output<<"changed"; }
        return {};
    }
    Result<void> commit() override {
        if (state_.mode==3) return make_resource_failure(ResourceFailure::receiver_commit_failed);
        state_.committed=true; return {};
    }
    void abort() noexcept override { state_.aborted=true; }
private: ReceiverState& state_;
};
class Receiver final : public PackageStreamReceiver {
public:
    explicit Receiver(ReceiverState& state) : state_(state) {}
    Result<std::unique_ptr<PackageStreamTransaction>> begin(const DeliveredMapPackage& package) override {
        if (package.manifest.files.size()!=2) return make_resource_failure(ResourceFailure::receiver_commit_failed);
        return std::unique_ptr<PackageStreamTransaction>(new Transaction(state_));
    }
private: ReceiverState& state_;
};
void receivers_and_outbox(const std::filesystem::path& root) {
    const auto value=fixture(); auto sink=prepared(root/"packages",value); EXPECT(sink); if (!sink) return;
    auto package=sink.value()->commit(); EXPECT(package); if (!package) return;
    for (int mode=0;mode<4;++mode) {
        ReceiverState state; state.mode=mode; Receiver receiver(state);
        auto result=deliver_package_to_transactional_stream(package.value(),receiver,3);
        EXPECT(static_cast<bool>(result)==(mode==0)); EXPECT(state.committed==(mode==0)); EXPECT(state.aborted==(mode!=0));
        if (mode==0) EXPECT(state.bytes==value.bytes);
        else EXPECT(result.error().server_code=="receiver_commit_failed");
    }
    CancellationSignal cancel; ReceiverState canceled; canceled.cancel=&cancel; Receiver receiver(canceled);
    EXPECT(!deliver_package_to_transactional_stream(package.value(),receiver,3,&cancel));
    EXPECT(canceled.aborted && !canceled.committed);
    bool called=false;
    auto callback=deliver_package_to_verified_callback(package.value(),[&](const DeliveredMapPackage& input) {
        called=true; return verify_map_package(input);
    }); EXPECT(callback); EXPECT(called);
    auto thrown=deliver_package_to_verified_callback(package.value(),[](const DeliveredMapPackage&) -> Result<void> {
        throw std::runtime_error("synthetic private receiver detail");
    }); EXPECT(!thrown); EXPECT(thrown.error().server_code=="receiver_commit_failed");
    auto store=DurableStateStore::open(root/"state"/"state.sqlite3"); EXPECT(store); if (!store) return;
    EXPECT(store.value()->register_task_generation({task_id,1},0));
    StatusEventRecord event; event.generation={task_id,1}; event.lease_token="synthetic-package-lease";
    event.lease_expires_at=4102444800; event.local_delivery_evidence=R"({"phase":"pending"})";
    for (auto state:{TaskExecutionState::downloading,TaskExecutionState::delivering}) {
        event.state=state; event.progress=20; event.event_id=generate_uuid_v4().value();
        event.payload_json=make_status_request(event).value(); EXPECT(store.value()->persist_status_event(event));
    }
    event.state=TaskExecutionState::delivered; event.progress=100; event.event_id=generate_uuid_v4().value();
    event.payload_json=make_status_request(event).value(); event.local_delivery_evidence=std::string(16385,'x');
    EXPECT(!store.value()->persist_status_event(event));
    EXPECT(store.value()->task_generation({task_id,1}).value().state==TaskExecutionState::delivering);
    EXPECT(!store.value()->has_unconfirmed_terminal_event(task_id).value());
    event.local_delivery_evidence=make_map_package_evidence(package.value(),event.generation).value();
    EXPECT(store.value()->persist_status_event(event));
    store.value().reset(); store=DurableStateStore::open(root/"state"/"state.sqlite3"); EXPECT(store);
    EXPECT(store.value()->status_event(event.event_id).value().local_delivery_evidence==event.local_delivery_evidence);
    EXPECT(store.value()->has_unconfirmed_terminal_event(task_id).value());
    ReceiverState mutation; mutation.mutation=package.value().directory/"yaml.bin"; Receiver mutating(mutation);
    EXPECT(!deliver_package_to_transactional_stream(package.value(),mutating,65536));
    EXPECT(!mutation.committed && mutation.aborted); EXPECT(!inspect_map_package(root/"packages",value.manifest));
}
void diagnostic_boundaries() {
    EXPECT(!classify_resource_failure({ErrorDomain::transport,ErrorCode::timeout,true,"synthetic"},true));
    EXPECT(!classify_resource_failure({ErrorDomain::http,ErrorCode::invalid_argument,false,"synthetic","task_not_found"},true));
    EXPECT(!classify_resource_failure({ErrorDomain::http,ErrorCode::invalid_argument,false,"synthetic","artifact_not_found"},false));
    EXPECT(classify_resource_failure({ErrorDomain::http,ErrorCode::invalid_argument,false,"synthetic","artifact_not_found"},true)
        ==ResourceFailure::artifact_file_missing);
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/"roc-vehicle-package-test";
    std::error_code ec; std::filesystem::remove_all(root,ec);
    for (const auto* format:{"png","jpeg","pgm-yaml"}) {
        successful_package(root/format,format); successful_package(root/(std::string{format}+"-worst"),format,true);
    }
    invalid_packages(root/"invalid"); missing_and_bad_files(root/"bad");
    absent_metadata(root/"null-metadata");
    permission_and_recovery(root/"shared"); receivers_and_outbox(root/"receivers"); diagnostic_boundaries();
    std::filesystem::remove_all(root,ec); return failures ? EXIT_FAILURE:EXIT_SUCCESS;
}
