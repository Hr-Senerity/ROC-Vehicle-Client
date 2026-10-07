// Test-only linker interposition. Production driver has no fault switch.
#include <sqlite3.h>
#include <cstring>
#include <cstdlib>
#include <csignal>
#include <unistd.h>
#include "roc/vehicle/device_client.hpp"
#include <iostream>
namespace {
// Sequence reservations may COMMIT on the DeviceClient I/O thread. A global
// armed flag would kill there before the worker's terminal transaction commits.
thread_local sqlite3* terminal_database=nullptr;
const char* window() {
    const char* value=std::getenv("ROC_VEHICLE_TEST_KILL_WINDOW");
    return value?value:"before_insert";
}
[[noreturn]] void kill_here(const char* point) {
    std::cout<<"test_probe checkpoint="<<point<<'\n'<<std::flush;
    static_cast<void>(::kill(::getpid(),SIGKILL));std::_Exit(77);
}
}
void roc_vehicle_test_online(roc::vehicle::DeviceClient& device) {
    const char* enabled=std::getenv("ROC_VEHICLE_TEST_SYNTHETIC_TELEMETRY");
    if(!enabled || std::strcmp(enabled,"1")!=0)return;
    roc::vehicle::TelemetrySnapshot sample;
    sample.online=true;sample.battery_level=100;
    sample.library_version="integration-synthetic-probe";
    const auto queued=device.publish_telemetry(std::move(sample));
    std::cout<<"test_probe synthetic_telemetry_queued="<<static_cast<bool>(queued)<<'\n'<<std::flush;
}
extern "C" int __real_sqlite3_step(sqlite3_stmt*);
extern "C" int __real_sqlite3_exec(sqlite3*,const char*,int(*)(void*,int,char**,char**),void*,char**);
extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    const auto* sql=sqlite3_sql(statement);
    if(sql && std::strstr(sql,"INSERT INTO status_events")) {
        char* expanded=sqlite3_expanded_sql(statement);
        const bool terminal=expanded && std::strstr(expanded,"'delivered'");
        if(expanded) {
            const auto length=std::strlen(expanded);
            auto* bytes=static_cast<volatile char*>(expanded);
            for(std::size_t i=0;i<length;++i)bytes[i]=0;
            sqlite3_free(expanded);
        }
        if(terminal && std::strcmp(window(),"before_insert")==0)kill_here("publication_before_terminal_insert");
        if(terminal && std::strcmp(window(),"after_commit")==0) {
            const int result=__real_sqlite3_step(statement);
            if(result==SQLITE_DONE)terminal_database=sqlite3_db_handle(statement);
            return result;
        }
    }
    return __real_sqlite3_step(statement);
}
extern "C" int __wrap_sqlite3_exec(sqlite3* db,const char* sql,int(*callback)(void*,int,char**,char**),void* data,char** error) {
    const int result=__real_sqlite3_exec(db,sql,callback,data,error);
    if(terminal_database==db && sql && std::strcmp(sql,"COMMIT")==0 && result==SQLITE_OK)
        kill_here("terminal_commit_before_http_send");
    if(terminal_database==db && sql && std::strcmp(sql,"ROLLBACK")==0)terminal_database=nullptr;
    return result;
}
