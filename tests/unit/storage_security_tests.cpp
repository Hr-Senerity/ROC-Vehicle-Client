#include "../road_test_support.hpp"
#include "../package_test_support.hpp"
#include "roc/vehicle/state_store.hpp"
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace roc::vehicle;
namespace {
int failures=0;
#define EXPECT(value) do { if (!(value)) { std::cerr<<__LINE__<<": "<<#value<<'\n';++failures; } } while(false)
void mode(const std::filesystem::path& path,mode_t expected) {
    struct stat data {};EXPECT(::lstat(path.c_str(),&data)==0);EXPECT((data.st_mode&07777)==expected);
}
void bytes(const std::filesystem::path& path,std::string_view text) {
    std::ofstream output(path,std::ios::binary);output<<text;
}
void sqlite_locks(const std::filesystem::path& base) {
    const auto database=base/"record-locks"/"state.sqlite3";
    auto store=DurableStateStore::open(database);EXPECT(store);if (!store) return;
    int descriptors[2]{-1,-1};
    const std::filesystem::path paths[2]{database,database.string()+"-shm"};
    for (int i=0;i<2;++i) {
        descriptors[i]=::open(paths[i].c_str(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);EXPECT(descriptors[i]>=0);
        struct flock lock {};lock.l_type=F_WRLCK;lock.l_whence=SEEK_SET;lock.l_start=400;lock.l_len=1;
        EXPECT(::fcntl(descriptors[i],F_SETLK,&lock)==0); // Outside SQLite's lock bytes.
    }
    EXPECT(store.value()->allocate_sequence()); // Revalidates DB/WAL/SHM permissions.
    const auto child=::fork();EXPECT(child>=0);
    if (child==0) {
        bool protected_locks=true;
        for (int i=0;i<2;++i) {
            struct flock query {};query.l_type=F_WRLCK;query.l_whence=SEEK_SET;query.l_start=400;query.l_len=1;
            protected_locks=protected_locks && ::fcntl(descriptors[i],F_GETLK,&query)==0 && query.l_type==F_WRLCK;
        }
        ::_exit(protected_locks?0:1);
    }
    if (child>0) {int status=0;EXPECT(::waitpid(child,&status,0)==child);EXPECT(WIFEXITED(status) && WEXITSTATUS(status)==0);}
    for (const int fd:descriptors) {
        struct flock lock {};lock.l_type=F_UNLCK;lock.l_whence=SEEK_SET;lock.l_start=400;lock.l_len=1;
        EXPECT(::fcntl(fd,F_SETLK,&lock)==0);
    }
    store.value().reset(); // Never close auxiliary DB/SHM descriptors while SQLite is live.
    for (const int fd:descriptors)if (fd>=0)EXPECT(::close(fd)==0);
}
void check(const std::filesystem::path& base) {
    const auto text=road_test::fixture().to_string();const auto manifest=road_test::manifest(text);
    const auto shared=base/"shared";std::filesystem::create_directories(shared);EXPECT(::chmod(shared.c_str(),0755)==0);
    EXPECT(!FileSink::create(shared,manifest));mode(shared,0755);
    EXPECT(!FileSink::create("/",manifest));EXPECT(!FileSink::create(std::filesystem::temp_directory_path(),manifest));
    auto sink=FileSink::create(shared/"vehicle",manifest);EXPECT(sink);
    if (sink) {
        mode(shared,0755);mode(shared/"vehicle",0700);mode(sink.value()->part_path(),0600);
        EXPECT(sink.value()->write(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
        EXPECT(sink.value()->seal(road_test::metadata(manifest)));auto saved=sink.value()->commit();EXPECT(saved);
        if (saved) { mode(saved.value().path,0600);EXPECT(inspect_file_artifact(shared/"vehicle",manifest)); }
    }
    const auto outside=shared/"outside";std::filesystem::create_directory(outside);mode(outside,0755);
    std::filesystem::create_directory_symlink(outside,shared/"alias");
    EXPECT(!FileSink::create(shared/"alias"/"nested"/"artifact",manifest));
    EXPECT(!DurableStateStore::open(shared/"alias"/"nested"/"state"/"state.sqlite3"));
    EXPECT(!PackageSink::create(shared/"alias"/"nested"/"package",package_test::fixture().manifest));
    EXPECT(!std::filesystem::exists(outside/"nested"));mode(outside,0755);
    const auto sentinel=outside/"ordinary";bytes(sentinel,"nonsecret sentinel");mode(sentinel,0644);
    // Refuse DB/WAL/SHM hard links before chmod or SQLite interprets the bytes.
    for (const std::string suffix:{"","-wal","-shm"}) {
        const auto state=shared/("state"+suffix);std::filesystem::create_directory(state);EXPECT(::chmod(state.c_str(),0700)==0);
        std::filesystem::create_hard_link(sentinel,state/("state.sqlite3"+suffix));
        EXPECT(!DurableStateStore::open(state/"state.sqlite3"));
        mode(sentinel,0644);EXPECT(road_test::read(sentinel)=="nonsecret sentinel");
    }
    for (const std::string suffix:{"","-wal","-shm"}) {
        const auto state=shared/("symlink-state"+suffix);std::filesystem::create_directory(state);EXPECT(::chmod(state.c_str(),0700)==0);
        std::filesystem::create_symlink(sentinel,state/("state.sqlite3"+suffix));
        EXPECT(!DurableStateStore::open(state/"state.sqlite3"));mode(sentinel,0644);
    }
    const auto cleanup=shared/"cleanup";auto prepared=FileSink::create(cleanup,manifest);EXPECT(prepared);
    if (prepared) {
        const auto generated=prepared.value()->part_path();prepared.value().reset();
        bytes(cleanup/".roc-unrelated.part","keep");
        std::filesystem::create_hard_link(sentinel,generated);
        EXPECT(!recover_file_sink_parts(cleanup));EXPECT(std::filesystem::exists(generated));
        std::filesystem::remove(generated);std::filesystem::create_symlink(sentinel,generated);
        EXPECT(!recover_file_sink_parts(cleanup));EXPECT(std::filesystem::is_symlink(generated));
        std::filesystem::remove(generated);bytes(generated,"partial");EXPECT(::chmod(generated.c_str(),0600)==0);
        auto removed=recover_file_sink_parts(cleanup);EXPECT(removed);if (removed) EXPECT(removed.value()==1);
        EXPECT(road_test::read(cleanup/".roc-unrelated.part")=="keep");
    }
    auto linked=FileSink::create(shared/"linked-final",manifest);EXPECT(linked);
    if (linked) {
        EXPECT(linked.value()->write(reinterpret_cast<const std::uint8_t*>(text.data()),text.size()));
        EXPECT(linked.value()->seal(road_test::metadata(manifest)));
        std::filesystem::create_hard_link(sentinel,linked.value()->final_path());
        EXPECT(!linked.value()->commit());mode(sentinel,0644);EXPECT(road_test::read(sentinel)=="nonsecret sentinel");
    }
    auto active=DurableStateStore::open(shared/"private-state"/"state.sqlite3");EXPECT(active);
    if (active) {
        mode(shared/"private-state",0700);mode(shared/"private-state"/"state.sqlite3",0600);
        mode(shared/"private-state"/"state.sqlite3-wal",0600);mode(shared/"private-state"/"state.sqlite3-shm",0600);
    }
    bytes(shared/"ordinary-after","ordinary");mode(shared/"ordinary-after",0644);mode(shared,0755);
}
}
int main() {
    const auto root=std::filesystem::temp_directory_path()/"roc-storage-security";
    std::error_code error;std::filesystem::remove_all(root,error);
    const auto original=::umask(0022); // Single-threaded fixture only; the library never calls umask.
    check(root);sqlite_locks(root);const auto observed=::umask(original);EXPECT(observed==0022);
    std::filesystem::remove_all(root,error);return failures?EXIT_FAILURE:EXIT_SUCCESS;
}
