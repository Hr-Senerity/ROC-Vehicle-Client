#include "roc/vehicle/result.hpp"
#include <jsoncons/json.hpp>
#include <filesystem>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

namespace {
using Json=jsoncons::json;
bool write(const std::filesystem::path& p,std::string_view bytes) {
    const int fd=::open(p.c_str(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0)return false;
    std::size_t offset=0;bool ok=true;
    while(offset<bytes.size()) {
        const auto n=::write(fd,bytes.data()+offset,bytes.size()-offset);
        if(n<=0){ok=false;break;}offset+=static_cast<std::size_t>(n);
    }
    ok=ok && ::fsync(fd)==0;if(::close(fd)!=0)ok=false;return ok;
}
bool mode(const std::filesystem::path& p,mode_t permissions,bool directory) {
    struct stat st{};return ::lstat(p.c_str(),&st)==0 && st.st_uid==::geteuid() &&
        (st.st_mode&07777)==permissions && (directory?S_ISDIR(st.st_mode):S_ISREG(st.st_mode));
}
}
int main(int argc,char** argv) {
    if(argc!=3)return 2;
    const std::filesystem::path root=argv[2];
    if(!root.is_absolute() || !mode(root,0700,true))return 2;
    if(std::string(argv[1])=="--create") {
        // Deliberately synthetic; no server, lease or actual credential exists.
        const auto config=Json::parse(R"({"base_url":"http://127.0.0.1:1","vehicle_id":"123e4567-e89b-42d3-a456-426614174001",
            "token_file":"/run/vehicle/device-token","state_directory":"/run/vehicle/state","artifact_directory":"/run/vehicle/artifacts",
            "transport":"http-ws-demo","network_scope":"loopback","offline_budget_ms":10000,
            "reconnect_initial_ms":100,"reconnect_maximum_ms":200,
            "owning_map_context_file":"/run/vehicle/context.json"})");
        const auto context=Json::parse(R"({"context_version":1,"system_commit":"a9c601d6977d89000438854a24e3199573004a2f",
            "vehicle_id":"123e4567-e89b-42d3-a456-426614174001","bindings":[{
            "project_id":"123e4567-e89b-12d3-a456-426614174012","resource_revision_id":"123e4567-e89b-12d3-a456-426614174013",
            "artifact_sha256":"0000000000000000000000000000000000000000000000000000000000000000",
            "map_id":"123e4567-e89b-12d3-a456-426614174014","map_revision_id":"123e4567-e89b-12d3-a456-426614174099",
            "coordinate_mode":"metric"}]})");
        if(!write(root/"config.json",config.to_string()) || !write(root/"context.json",context.to_string()) ||
           !write(root/"device-token","synthetic-runtime-smoke-token"))return 2;
    } else if(std::string(argv[1])=="--verify") {
        for(const auto* leaf:{"state","artifacts"}) {
            if(!mode(root/leaf,0700,true) || !mode(root/leaf/".roc-driver.lock",0600,false) ||
               !mode(root/leaf/"driver-binding.json",0600,false))return 2;
        }
        if(!mode(root/"state"/"roc-vehicle-state.sqlite3",0600,false))return 2;
        for(const auto* suffix:{"-wal","-shm"}) {
            const auto p=root/"state"/(std::string("roc-vehicle-state.sqlite3")+suffix);
            if(std::filesystem::exists(p) && !mode(p,0600,false))return 2;
        }
    } else return 2;
    std::cout<<"runtime_synthetic_inputs_verified_no_secrets\n";return 0;
}
