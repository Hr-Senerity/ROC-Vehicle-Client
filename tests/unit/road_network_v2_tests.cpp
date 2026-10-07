#include "../road_test_support.hpp"
#include "roc/vehicle/road_receiver.hpp"
#include "roc/vehicle/resource_failure.hpp"
#include "contract_validation.hpp"
#include <cstdlib>
#include <functional>
#include <iostream>
#include <limits>

using namespace roc::vehicle;
using namespace road_test;
namespace {
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n'; ++failures; } } while(false)
struct StreamState {
    std::string bytes; bool began{false},committed{false},aborted{false},fail_commit{false};
};
class StreamTransaction final:public ArtifactStreamTransaction {
public:
    explicit StreamTransaction(StreamState& state):state_(state) {}
    Result<void> write(const std::uint8_t* data,std::size_t size) override {
        state_.bytes.append(reinterpret_cast<const char*>(data),size);return {};
    }
    Result<void> commit() override {
        if (state_.fail_commit) return make_resource_failure(ResourceFailure::receiver_commit_failed);
        state_.committed=true;return {};
    }
    void abort() noexcept override { state_.aborted=true; }
private:
    StreamState& state_;
};
class StreamReceiver final:public TransactionalStreamReceiver {
public:
    explicit StreamReceiver(StreamState& state):state_(state) {}
    Result<std::unique_ptr<ArtifactStreamTransaction>> begin(const DeliveredArtifact& artifact) override {
        EXPECT(artifact.schema_version==2);state_.began=true;
        return std::unique_ptr<ArtifactStreamTransaction>{new StreamTransaction{state_}};
    }
private:
    StreamState& state_;
};
void compare(const RoadNetworkV2& typed,const Json& original) {
    EXPECT(typed.coordinate_mode==original["coordinate_mode"].as<std::string>());
    EXPECT(typed.nodes.front().id==original["nodes"][0]["id"].as<std::string>());
    EXPECT(typed.sampling.algorithm=="uniform-parameter-v1" && typed.sampling.precision==6);
    EXPECT(typed.sampling.spacing==original["sampling"]["spacing"].as<double>());
    EXPECT(typed.trajectories.size()==original["trajectories"].size());
    for (std::size_t t=0;t<typed.trajectories.size();++t) {
        const auto& a=typed.trajectories[t]; const auto& b=original["trajectories"][t];
        EXPECT(a.id==b["id"].as<std::string>() && a.edge_id==b["edge_id"].as<std::string>());
        EXPECT(a.direction==b["direction"].as<std::string>() && a.length==b["length"].as<double>());
        EXPECT(a.points.size()==b["points"].size());
        for (std::size_t i=0;i<a.points.size();++i) {
            const auto& p=a.points[i]; const auto& q=b["points"][i];
            EXPECT(p.index==q["index"].as<std::size_t>() && p.x==q["x"].as<double>() && p.y==q["y"].as<double>());
            EXPECT(p.s==q["s"].as<double>() && p.heading==q["heading"].as<double>());
        }
    }
}
void positive() {
    for (bool both:{false,true}) for (bool curve:{false,true}) {
        auto original=fixture(both,curve); auto parsed=parse_road_network_v2(original.to_string(),{"metric"});
        EXPECT(parsed); if (parsed) {
            compare(parsed.value(),original);
            EXPECT(parsed.value().edges[0].geometry.type==(curve?"cubic_bezier":"line"));
            if (curve) EXPECT(parsed.value().edges[0].geometry.control1->y==1);
        }
    }
    auto empty=fixture(); empty["nodes"]=Json(jsoncons::json_array_arg);
    empty["edges"]=Json(jsoncons::json_array_arg); empty["trajectories"]=Json(jsoncons::json_array_arg);
    EXPECT(parse_road_network_v2(empty.to_string()));
    auto zero=fixture(true,false); zero["nodes"][0]["x"]=0;
    zero["coordinate_mode"]="legacy-normalized";
    for (auto& trajectory:zero["trajectories"].array_range()) {
        trajectory["length"]=0; trajectory["max_speed_mps"]=Json::null();
        for (auto& point:trajectory["points"].array_range()) { point["x"]=0; point["s"]=0; }
    }
    zero["edges"][0]["max_speed_mps"]=Json::null(); EXPECT(parse_road_network_v2(zero.to_string()));
    auto outside=fixture(false,false); outside["coordinate_mode"]="legacy-normalized";
    EXPECT(parse_road_network_v2(outside.to_string())); // x=2 is legal, no clipping to [0,1].
    auto opposite=fixture(false,false); auto edge=opposite["edges"][0]; edge["id"]="back";
    edge["from"]="b"; edge["to"]="a"; opposite["edges"].push_back(edge);
    auto reverse=fixture(true,false)["trajectories"][1]; reverse["id"]="back:forward";
    reverse["edge_id"]="back"; reverse["direction"]="forward"; opposite["trajectories"].push_back(reverse);
    EXPECT(parse_road_network_v2(opposite.to_string()));
}
void negatives() {
    const std::vector<std::function<void(Json&)>> mutations={
        [](Json& j){j["nodes"].push_back(j["nodes"][0]);},
        [](Json& j){j["edges"].push_back(j["edges"][0]);},
        [](Json& j){j["edges"][0]["from"]="missing";},
        [](Json& j){j["edges"][0]["to"]="a";},
        [](Json& j){auto e=j["edges"][0];e["id"]="other";e["direction"]="forward";j["edges"].push_back(e);},
        [](Json& j){j["trajectories"].push_back(j["trajectories"][0]);},
        [](Json& j){j["trajectories"][1]["id"]=j["trajectories"][0]["id"];},
        [](Json& j){j["trajectories"][1]["edge_id"]="missing";},
        [](Json& j){j["trajectories"][1]["direction"]="forward";},
        [](Json& j){j["trajectories"].erase(j["trajectories"].array_range().begin()+1);},
        [](Json& j){j["trajectories"][0]["points"][1]["index"]=2;},
        [](Json& j){j["trajectories"][0]["points"][0]["x"]=1;},
        [](Json& j){j["trajectories"][1]["points"][2]["x"]=1;},
        [](Json& j){j["trajectories"][0]["points"][0]["s"]=0.1;},
        [](Json& j){j["trajectories"][0]["points"][2]["s"]=1;},
        [](Json& j){j["trajectories"][0]["length"]=3;},
        [](Json& j){j["trajectories"][0]["max_speed_mps"]=1;},
        [](Json& j){j["sampling"]["algorithm"]="unknown";},
        [](Json& j){j["sampling"]["precision"]=5;},
        [](Json& j){j["sampling"]["spacing"]=0;},
        [](Json& j){j["coordinate_mode"]="legacy-normalized";j["sampling"]["spacing"]=2;},
        [](Json& j){j["edges"][0]["geometry"]["control1"]["x"]=std::numeric_limits<double>::infinity();},
        [](Json& j){j["trajectories"][0]["points"][1]["heading"]=std::numeric_limits<double>::quiet_NaN();},
        [](Json& j){j["unexpected"]=1;},
        [](Json& j){j["edges"][0]["geometry"]["type"]="arc";},
        [](Json& j){j["trajectories"][0]["points"][1]["unexpected"]=1;}
    };
    for (const auto& mutate:mutations) {
        auto input=fixture(); mutate(input); const auto result=parse_road_network_v2(input.to_string());
        EXPECT(!result); if (!result) EXPECT(result.error().server_code=="road_network_invalid");
    }
    auto version=fixture(); version["schema_version"]=3; auto unsupported=parse_road_network_v2(version.to_string());
    EXPECT(!unsupported); EXPECT(unsupported.error().server_code=="unsupported_schema_version");
    auto mismatch=parse_road_network_v2(fixture().to_string(),{"legacy-normalized"});
    EXPECT(!mismatch); EXPECT(mismatch.error().server_code=="artifact_metadata_mismatch");
    const auto text=fixture().to_string(); EXPECT(!parse_road_network_v2(text,{"",text.size()-1}));
    auto overflow=text; const auto offset=overflow.find("0.75"); overflow.replace(offset,4,"1e400");
    EXPECT(!parse_road_network_v2(overflow));
    EXPECT(!parse_road_network_v2(std::string(40,'[')+"0"+std::string(40,']')));
    auto duplicate=text; duplicate.insert(1,"\"schema_version\":2,"); EXPECT(!parse_road_network_v2(duplicate));
}
void unicode_v1() {
    auto old=Json::parse(R"({"schema_version":1,"coordinate_mode":"metric","nodes":[],"edges":[]})");
    auto node=fixture()["nodes"][0]; std::string label;
    for (int i=0;i<128;++i) label+="\xE8\xBD\xA6";
    node["label"]=label; old["nodes"].push_back(node); EXPECT(validate_road_network_v1(old.to_string()));
    old["nodes"][0]["label"]=label+"\xE8\xBD\xA6"; EXPECT(!validate_road_network_v1(old.to_string()));
}
void limits() {
    // Fast preflight boundaries are distinct from complete-schema acceptance.
    for (const auto& item:std::vector<std::pair<std::string,std::size_t>>{{"nodes",10000},{"edges",50000},{"trajectories",100000},{"points",10000}}) {
        std::string text="{\""+item.first+"\":[";
        for (std::size_t i=0;i<item.second;++i) { if (i) text+=','; text+="{}"; }
        text+="]}"; EXPECT(detail::bounded_road_document(text,64U*1024U*1024U));
        text.insert(text.size()-2,",{}"); EXPECT(!detail::bounded_road_document(text,64U*1024U*1024U));
    }
    auto nodes=fixture();nodes["nodes"]=Json(jsoncons::json_array_arg);
    nodes["edges"]=Json(jsoncons::json_array_arg);nodes["trajectories"]=Json(jsoncons::json_array_arg);
    for (int i=0;i<10000;++i) { auto n=fixture()["nodes"][0];n["id"]="n"+std::to_string(i);nodes["nodes"].push_back(n); }
    EXPECT(parse_road_network_v2(nodes.to_string()));
    auto n=nodes["nodes"][0];n["id"]="extra";nodes["nodes"].push_back(n);EXPECT(!parse_road_network_v2(nodes.to_string()));
    // Complete valid network at the single-trajectory and total point limits.
    auto input=fixture(false,false); input["nodes"]=Json(jsoncons::json_array_arg);
    input["edges"]=Json(jsoncons::json_array_arg); input["trajectories"]=Json(jsoncons::json_array_arg);
    for (int edge_index=0;edge_index<20;++edge_index) {
        auto a=fixture()["nodes"][1]; auto b=fixture()["nodes"][0]; a["id"]="a"+std::to_string(edge_index);
        b["id"]="b"+std::to_string(edge_index); b["x"]=0; input["nodes"].push_back(a); input["nodes"].push_back(b);
        auto edge=fixture(false,false)["edges"][0]; edge["id"]="e"+std::to_string(edge_index);
        edge["from"]=a["id"];edge["to"]=b["id"];input["edges"].push_back(edge);
        auto t=fixture(false,false)["trajectories"][0]; t["id"]=edge["id"].as<std::string>()+":forward";
        t["edge_id"]=edge["id"]; t["length"]=0; t["points"]=Json(jsoncons::json_array_arg);
        for (int i=0;i<10000;++i) {
            auto p=Json::parse(R"({"index":0,"x":0,"y":0,"s":0,"heading":0})");p["index"]=i;t["points"].push_back(p);
        }
        input["trajectories"].push_back(std::move(t));
    }
    auto accepted=parse_road_network_v2(input.to_string()); EXPECT(accepted);
    if (accepted) EXPECT(accepted.value().trajectories.size()==20 && accepted.value().trajectories.back().points.size()==10000);
    auto extra=input["trajectories"][0]; extra["id"]="extra";
    extra["points"].erase(extra["points"].array_range().begin()+2,extra["points"].array_range().end());
    input["trajectories"][0]["points"].erase(input["trajectories"][0]["points"].array_range().end()-1);
    input["trajectories"].push_back(extra);
    // Exactly 200001 total; each trajectory remains within its own size cap.
    EXPECT(!detail::bounded_road_document(input.to_string(),64U*1024U*1024U));
    EXPECT(!parse_road_network_v2(input.to_string()));
    input["trajectories"].erase(input["trajectories"].array_range().begin()+20);
    auto point=input["trajectories"][1]["points"][0];point["index"]=10000;
    input["trajectories"][1]["points"].push_back(point); EXPECT(!parse_road_network_v2(input.to_string()));
}
void file_delivery(const std::filesystem::path& root) {
    const auto source=fixture(); const auto text=source.to_string(); const auto m=manifest(text);
    auto sink=FileSink::create(root,m,{"metric"}); EXPECT(sink); if (!sink) return;
    EXPECT(sink.value()->write(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
    EXPECT(sink.value()->seal(metadata(m))); auto delivered=sink.value()->commit(); EXPECT(delivered); if (!delivered) return;
    EXPECT(delivered.value().schema_version==2); EXPECT(read(delivered.value().path)==text);
    bool called=false; auto receipt=deliver_road_network_v2_to_callback(delivered.value(),[&](const DeliveredRoadNetworkV2& value) -> Result<void> {
        called=true; compare(value.network,source); EXPECT(read(value.artifact.path)==text); return {};
    },{"metric"}); EXPECT(receipt); EXPECT(called);
    StreamState streamed;StreamReceiver stream_receiver(streamed);
    EXPECT(deliver_to_transactional_stream(delivered.value(),stream_receiver,7));
    EXPECT(streamed.began && streamed.committed && !streamed.aborted && streamed.bytes==text);
    StreamState refused;refused.fail_commit=true;StreamReceiver refusing_receiver(refused);
    EXPECT(!deliver_to_transactional_stream(delivered.value(),refusing_receiver,7));
    EXPECT(refused.began && !refused.committed && refused.aborted);
    auto failed=deliver_road_network_v2_to_callback(delivered.value(),[](const DeliveredRoadNetworkV2&) -> Result<void> {
        throw std::runtime_error("synthetic private data");
    }); EXPECT(!failed); EXPECT(failed.error().server_code=="receiver_commit_failed");
    CancellationSignal cancel;cancel.cancel();called=false;
    EXPECT(!deliver_road_network_v2_to_callback(delivered.value(),[&](const DeliveredRoadNetworkV2&) -> Result<void>{called=true;return {};},{},&cancel));
    EXPECT(!called);
    auto invalid=fixture();invalid["trajectories"][0]["points"][1]["index"]=0;
    auto badtext=invalid.to_string();auto badmanifest=manifest(badtext);auto bad=FileSink::create(root/"invalid",badmanifest);EXPECT(bad);
    if (bad) { EXPECT(bad.value()->write(reinterpret_cast<const std::uint8_t*>(badtext.data()),badtext.size()));
        EXPECT(!bad.value()->seal(metadata(badmanifest)));EXPECT(!bad.value()->commit()); }
    auto unknown=m;unknown.schema_version=3;EXPECT(!FileSink::create(root/"unknown",unknown));
    auto mime=m;mime.content_type="application/json";EXPECT(!FileSink::create(root/"mime",mime));
    // Exercise v2 metadata/length/header gates, not only their inherited v1 cases.
    for (int fault=0;fault<4;++fault) {
        auto rejected=FileSink::create(root/("metadata-"+std::to_string(fault)),m);EXPECT(rejected);
        if (!rejected) continue;
        const auto amount=fault==3?text.size()-1:text.size();
        EXPECT(rejected.value()->write(reinterpret_cast<const std::uint8_t*>(text.data()),amount));
        auto received=metadata(m);
        if (fault==0) received.sha256=std::string(64,'0');
        if (fault==1) received.content_length=m.byte_size+1;
        if (fault==2) received.bytes_received=m.byte_size-1;
        auto refusal=rejected.value()->seal(received);EXPECT(!refusal);
        if (!refusal) EXPECT(refusal.error().server_code==(fault==0?"artifact_hash_mismatch":"artifact_metadata_mismatch"));
        EXPECT(!rejected.value()->commit());
    }
    auto forged=delivered.value();forged.schema_version=1;EXPECT(!load_verified_road_network_v2(forged));
    auto mutated=FileSink::create(root/"changed-part",m);EXPECT(mutated);
    if (mutated) {
        EXPECT(mutated.value()->write(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
        EXPECT(mutated.value()->seal(metadata(m)));auto changed=text;changed[changed.find("0.75")]='9';
        {std::ofstream out(mutated.value()->part_path(),std::ios::binary);out<<changed;}
        EXPECT(!mutated.value()->commit());
    }
    // A matching hash alone cannot turn an invalid road into a receiver input.
    auto forged_road=delivered.value();
    {std::ofstream out(forged_road.path,std::ios::binary);out<<badtext;}
    forged_road.sha256=sha256_hex(badtext).value();forged_road.byte_size=badtext.size();called=false;
    EXPECT(!deliver_to_verified_file_callback(forged_road,[&](const DeliveredArtifact&)->Result<void>{called=true;return {};}));
    EXPECT(!called);
    StreamState unverified;StreamReceiver unverified_receiver(unverified);
    EXPECT(!deliver_to_transactional_stream(forged_road,unverified_receiver));EXPECT(!unverified.began);
    { std::ofstream out(delivered.value().path,std::ios::binary);out<<"changed"; }
    EXPECT(!load_verified_road_network_v2(delivered.value()));
}
}
int main(int argc,char**) {
    if (argc>1) { limits();return failures?EXIT_FAILURE:EXIT_SUCCESS; }
    positive();negatives();unicode_v1();
    const auto root=std::filesystem::temp_directory_path()/"roc-road-v2-test";
    std::error_code ec;std::filesystem::remove_all(root,ec);file_delivery(root);std::filesystem::remove_all(root,ec);
    return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
