// Synthetic SQLite only: the test-only crash boundary cannot be stolen by
// another connection/thread COMMIT, nor remain armed after ROLLBACK.
#include <sqlite3.h>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
namespace {
void check(bool value) {if(!value)throw std::runtime_error("probe_boundary_assertion");}
sqlite3* database() {
    sqlite3* db=nullptr;check(sqlite3_open(":memory:",&db)==SQLITE_OK);
    check(sqlite3_exec(db,"CREATE TABLE status_events(state TEXT)",nullptr,nullptr,nullptr)==SQLITE_OK);
    return db;
}
void exec(sqlite3* db,const char* sql) {check(sqlite3_exec(db,sql,nullptr,nullptr,nullptr)==SQLITE_OK);}
void insert(sqlite3* db) {
    sqlite3_stmt* stmt=nullptr;check(sqlite3_prepare_v2(db,"INSERT INTO status_events VALUES ('delivered')",-1,&stmt,nullptr)==SQLITE_OK);
    check(sqlite3_step(stmt)==SQLITE_DONE);check(sqlite3_finalize(stmt)==SQLITE_OK);
}
}
int main() {
    try {
        check(::setenv("ROC_VEHICLE_TEST_KILL_WINDOW","after_commit",1)==0);
        auto* a=database();auto* b=database();exec(a,"BEGIN");insert(a);
        exec(b,"BEGIN");exec(b,"COMMIT"); // same thread, different connection
        std::thread io([]{auto* db=database();exec(db,"BEGIN");exec(db,"COMMIT");check(sqlite3_close(db)==SQLITE_OK);});
        io.join();exec(a,"ROLLBACK");exec(a,"BEGIN");exec(a,"COMMIT");
        check(sqlite3_close(a)==SQLITE_OK);check(sqlite3_close(b)==SQLITE_OK);
        const auto pid=::fork();check(pid>=0);
        if(pid==0) {auto* db=database();exec(db,"BEGIN");insert(db);exec(db,"COMMIT");std::_Exit(99);}
        int status=0;check(::waitpid(pid,&status,0)==pid);check(WIFSIGNALED(status) && WTERMSIG(status)==SIGKILL);
        check(::unsetenv("ROC_VEHICLE_TEST_KILL_WINDOW")==0);
        std::cout<<"probe boundary other_connection_commit=ignored other_thread_commit=ignored rollback=disarmed exact_commit=SIGKILL\n";
        return 0;
    }catch(...) {std::cerr<<"probe_boundary_failed\n";return 1;}
}
