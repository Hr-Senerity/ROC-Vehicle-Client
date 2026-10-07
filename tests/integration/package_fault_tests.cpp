#include "../package_test_support.hpp"
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>

using namespace roc::vehicle;
using namespace package_test;
namespace {
enum class Action { none,fail,kill_before,kill_after };
std::atomic<int> fsync_countdown{0};
std::atomic<int> write_countdown{0};
std::atomic<Action> fsync_action{Action::none},rename_action{Action::none};
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n'; ++failures; } } while(false)
[[noreturn]] void strong_kill() { ::kill(::getpid(),SIGKILL); std::_Exit(90); }
void reset_faults() { fsync_countdown=0; write_countdown=0; fsync_action=Action::none; rename_action=Action::none; }
bool killed(pid_t child) {
    int status=0; return ::waitpid(child,&status,0)==child && WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL;
}
Result<std::unique_ptr<PackageSink>> prepared_faulted(
    const std::filesystem::path& root,const Fixture& value,int fsync_call=0,Action action=Action::none) {
    auto sink=PackageSink::create(root,value.manifest); if (!sink) return sink.error();
    fsync_countdown=fsync_call; fsync_action=action;
    for (auto it=value.manifest.files.rbegin();it!=value.manifest.files.rend();++it) {
        const auto sealed=write_and_seal(*sink.value(),value,*it); if (!sealed) return sealed.error();
    }
    const auto sealed=sink.value()->seal(); if (!sealed) return sealed.error();
    return sink;
}
void fsync_failure(const std::filesystem::path& root,int call) {
    reset_faults();
    // Separate directory-provisioning durability from the seven package barriers.
    auto value=fixture(); auto sink=prepared_faulted(root,value,call,Action::fail);
    if (call<=4) {
        EXPECT(!sink);
    } else {
        EXPECT(sink); if (sink) EXPECT(!sink.value()->commit());
    }
    reset_faults();
    if (call<=4) { EXPECT(published_count(root)==0); }
    else {
        // Publication happened, but no delivery receipt was returned. Keep and recover it.
        EXPECT(published_count(root)==1);
        EXPECT(inspect_map_package(root,value.manifest));
    }
    if (sink) sink.value().reset();
    EXPECT(recover_package_staging(root).value()==0);
}
void creation_failure(const std::filesystem::path& root) {
    reset_faults(); fsync_countdown=1; fsync_action=Action::fail;
    const auto sink=PackageSink::create(root,fixture().manifest);
    EXPECT(!sink); if (!sink) EXPECT(sink.error().server_code=="local_storage_failure");
    reset_faults(); EXPECT(published_count(root)==0);
    EXPECT(recover_package_staging(root).value()==0);
}
void write_failure(const std::filesystem::path& root,int call) {
    reset_faults(); write_countdown=call;
    auto sink=prepared(root,fixture()); EXPECT(!sink);
    if (!sink) EXPECT(sink.error().server_code=="local_storage_failure");
    reset_faults(); EXPECT(published_count(root)==0);
    EXPECT(recover_package_staging(root).value()==0);
}
void rename_failure(const std::filesystem::path& root) {
    const auto value=fixture(); auto sink=prepared(root,value); EXPECT(sink); if (!sink) return;
    rename_action=Action::fail; auto committed=sink.value()->commit(); EXPECT(!committed);
    EXPECT(committed.error().server_code=="local_storage_failure"); EXPECT(published_count(root)==0);
    sink.value().reset(); EXPECT(recover_package_staging(root).value()==0);
}
void crash(const std::filesystem::path& root,int boundary) {
    const auto value=fixture();
    auto provision=PackageSink::create(root,value.manifest); EXPECT(provision); if (!provision) return;
    provision.value().reset();
    const pid_t child=::fork(); EXPECT(child>=0);
    if (child==0) {
        reset_faults();
        if (boundary==0) {
            auto sink=PackageSink::create(root,value.manifest);
            if (!sink || !write_and_seal(*sink.value(),value,value.manifest.files[0])) std::_Exit(10);
            strong_kill();
        }
        auto sink=prepared_faulted(root,value,boundary==1?4:0,
            boundary==1?Action::kill_before:Action::none); if (!sink) std::_Exit(11);
        if (boundary==2) rename_action=Action::kill_before;
        if (boundary==3) rename_action=Action::kill_after;
        if (boundary==4) { fsync_countdown=1; fsync_action=Action::kill_before; }
        static_cast<void>(sink.value()->commit()); std::_Exit(12);
    }
    if (child<0) return;
    EXPECT(killed(child)); reset_faults();
    const bool published=boundary>=3;
    EXPECT(published_count(root)==(published?1U:0U));
    const auto inspected=inspect_map_package(root,value.manifest);
    EXPECT(static_cast<bool>(inspected)==published);
    const auto recovered=recover_package_staging(root); EXPECT(recovered);
    if (recovered) EXPECT(recovered.value()==(published?0U:1U));
    auto retry=prepared(root,value); EXPECT(retry);
    if (retry) {
        auto receipt=retry.value()->commit(); EXPECT(receipt); EXPECT(published_count(root)==1);
        if (receipt) EXPECT(make_map_package_evidence(receipt.value(),{task_id,1}));
    }
}
void concurrent_commit(const std::filesystem::path& root) {
    const auto value=fixture(); auto a=prepared(root,value); auto b=prepared(root,value);
    EXPECT(a && b); if (!a || !b) return;
    bool first=false,second=false;
    std::thread worker([&] { first=static_cast<bool>(a.value()->commit()); });
    second=static_cast<bool>(b.value()->commit()); worker.join();
    EXPECT(first && second); EXPECT(published_count(root)==1); EXPECT(recover_package_staging(root).value()==0);
}
}
extern "C" int __real_fsync(int fd);
extern "C" ssize_t __real_write(int fd,const void* bytes,size_t size);
extern "C" ssize_t __wrap_write(int fd,const void* bytes,size_t size) {
    if (write_countdown.load()>0 && write_countdown.fetch_sub(1)==1) { errno=ENOSPC; return -1; }
    return __real_write(fd,bytes,size);
}
extern "C" int __real_renameat2(int old_fd,const char* old_name,int new_fd,const char* new_name,unsigned flags);
extern "C" int __wrap_fsync(int fd) {
    if (fsync_countdown.load()>0 && fsync_countdown.fetch_sub(1)==1) {
        const auto action=fsync_action.exchange(Action::none);
        if (action==Action::kill_before) strong_kill();
        if (action==Action::fail) { errno=EIO; return -1; }
    }
    return __real_fsync(fd);
}
extern "C" int __wrap_renameat2(int old_fd,const char* old_name,int new_fd,const char* new_name,unsigned flags) {
    const auto action=rename_action.exchange(Action::none);
    if (action==Action::kill_before) strong_kill();
    if (action==Action::fail) { errno=EIO; return -1; }
    const auto result=__real_renameat2(old_fd,old_name,new_fd,new_name,flags);
    if (action==Action::kill_after && result==0) strong_kill();
    return result;
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/"roc-vehicle-package-faults";
    std::error_code ec; std::filesystem::remove_all(root,ec);
    creation_failure(root/"creation");
    for (int call=1;call<=3;++call) write_failure(root/("write-"+std::to_string(call)),call);
    for (int call=1;call<=7;++call) fsync_failure(root/("fsync-"+std::to_string(call)),call);
    rename_failure(root/"rename");
    for (int boundary=0;boundary<=4;++boundary) crash(root/("kill-"+std::to_string(boundary)),boundary);
    concurrent_commit(root/"concurrent");
    std::filesystem::remove_all(root,ec); return failures ? EXIT_FAILURE:EXIT_SUCCESS;
}
