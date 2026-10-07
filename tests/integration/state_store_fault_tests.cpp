#include "roc/vehicle/state_store.hpp"
#include "roc/vehicle/task_protocol.hpp"

#include <sqlite3.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {

enum class Target : int { none, acknowledge, resolve };
enum class Timing : int { before_step, after_step };

std::atomic<int> fault_target{static_cast<int>(Target::none)};
std::atomic<int> fault_timing{static_cast<int>(Timing::before_step)};
int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: "
                  << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

constexpr auto kTaskId = "123e4567-e89b-12d3-a456-426614174010";
constexpr auto kEventId = "123e4567-e89b-12d3-a456-426614174043";
constexpr auto kLease = "opaque-database-fault-lease";
constexpr auto kEvidence =
    R"JSON({"receiver":"file","fsync":true,"database_fault_test":true})JSON";

roc::vehicle::StatusEventRecord make_event(
    std::string event_id,
    roc::vehicle::TaskExecutionState state,
    std::int64_t progress) {
    roc::vehicle::StatusEventRecord event{
        std::move(event_id), {kTaskId, 1}, state, progress, {}, {}, {}, {},
        kLease, 4102444800, kEvidence};
    const auto payload = roc::vehicle::make_status_request(event);
    if (payload) {
        event.payload_json = payload.value();
    }
    return event;
}

bool persist_terminal_event(roc::vehicle::DurableStateStore& store) {
    auto downloading = make_event(
        "123e4567-e89b-12d3-a456-426614174041",
        roc::vehicle::TaskExecutionState::downloading, 20);
    auto delivering = make_event(
        "123e4567-e89b-12d3-a456-426614174042",
        roc::vehicle::TaskExecutionState::delivering, 80);
    auto delivered = make_event(
        kEventId, roc::vehicle::TaskExecutionState::delivered, 100);
    return store.register_task_generation({kTaskId, 1}, 20) &&
           !downloading.payload_json.empty() &&
           store.persist_status_event(downloading) &&
           store.acknowledge_status_event(downloading.event_id) &&
           store.persist_status_event(delivering) &&
           store.acknowledge_status_event(delivering.event_id) &&
           store.persist_status_event(delivered);
}

void arm_fault(Target target, Timing timing) {
    fault_timing.store(static_cast<int>(timing));
    fault_target.store(static_cast<int>(target));
}

void verify_blocked_event(const std::filesystem::path& database,
                          roc::vehicle::StatusEventDisposition disposition) {
    auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(reopened);
    if (!reopened) {
        return;
    }
    const auto event = reopened.value()->status_event(kEventId);
    EXPECT(event);
    if (event) {
        EXPECT(event.value().disposition == disposition);
        EXPECT(event.value().lease_token == kLease);
        EXPECT(event.value().local_delivery_evidence == kEvidence);
        EXPECT(event.value().request_sha256.size() == 64);
    }
    const auto blocked = reopened.value()->has_unconfirmed_terminal_event(kTaskId);
    EXPECT(blocked);
    if (blocked) {
        EXPECT(blocked.value());
    }
}

void verify_released_event(const std::filesystem::path& database,
                           roc::vehicle::StatusEventDisposition disposition) {
    auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(reopened);
    if (!reopened) {
        return;
    }
    const auto event = reopened.value()->status_event(kEventId);
    EXPECT(event);
    if (event) {
        EXPECT(event.value().disposition == disposition);
        EXPECT(event.value().lease_token.empty());
        EXPECT(event.value().local_delivery_evidence == kEvidence);
    }
    const auto blocked = reopened.value()->has_unconfirmed_terminal_event(kTaskId);
    EXPECT(blocked);
    if (blocked) {
        EXPECT(!blocked.value());
    }
}

void test_acknowledge_io_error(Timing timing,
                               const std::filesystem::path& root) {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto database = root / "state" / "state.sqlite3";
    {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(opened);
        if (!opened) {
            return;
        }
        EXPECT(persist_terminal_event(*opened.value()));
        arm_fault(Target::acknowledge, timing);
        EXPECT(!opened.value()->acknowledge_status_event(kEventId));
        EXPECT(fault_target.load() == static_cast<int>(Target::none));
    }
    if (timing == Timing::before_step) {
        verify_blocked_event(
            database, roc::vehicle::StatusEventDisposition::pending);
    } else {
        verify_released_event(
            database, roc::vehicle::StatusEventDisposition::acknowledged);
    }
    std::filesystem::remove_all(root, ignored);
}

void test_resolve_io_error(Timing timing,
                           const std::filesystem::path& root) {
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto database = root / "state" / "state.sqlite3";
    {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(opened);
        if (!opened) {
            return;
        }
        EXPECT(persist_terminal_event(*opened.value()));
        EXPECT(opened.value()->mark_status_event_reconciliation_required(
            kEventId, "database_fault_test"));
        arm_fault(Target::resolve, timing);
        EXPECT(!opened.value()->resolve_status_event_reconciliation(kEventId));
        EXPECT(fault_target.load() == static_cast<int>(Target::none));
    }
    if (timing == Timing::before_step) {
        verify_blocked_event(
            database,
            roc::vehicle::StatusEventDisposition::reconciliation_required);
    } else {
        verify_released_event(
            database, roc::vehicle::StatusEventDisposition::resolved);
    }
    std::filesystem::remove_all(root, ignored);
}

bool statement_matches(sqlite3_stmt* statement, Target target) {
    const char* sql = sqlite3_sql(statement);
    if (sql == nullptr) {
        return false;
    }
    const std::string_view text{sql};
    if (target == Target::acknowledge) {
        return text.find("disposition = 'acknowledged'") !=
               std::string_view::npos;
    }
    if (target == Target::resolve) {
        return text.find("disposition = 'resolved'") != std::string_view::npos;
    }
    return false;
}

}  // namespace

extern "C" int __real_sqlite3_step(sqlite3_stmt* statement);

extern "C" int __wrap_sqlite3_step(sqlite3_stmt* statement) {
    const auto target = static_cast<Target>(fault_target.load());
    if (target == Target::none || !statement_matches(statement, target)) {
        return __real_sqlite3_step(statement);
    }
    fault_target.store(static_cast<int>(Target::none));
    const auto timing = static_cast<Timing>(fault_timing.load());
    if (timing == Timing::before_step) {
        return SQLITE_IOERR_WRITE;
    }
    const int result = __real_sqlite3_step(statement);
    return result == SQLITE_DONE ? SQLITE_IOERR_WRITE : result;
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-state-store-fault-test";
    test_acknowledge_io_error(Timing::before_step, root / "ack-before");
    test_acknowledge_io_error(Timing::after_step, root / "ack-after");
    test_resolve_io_error(Timing::before_step, root / "resolve-before");
    test_resolve_io_error(Timing::after_step, root / "resolve-after");
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
