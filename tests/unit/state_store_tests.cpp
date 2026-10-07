#include "roc/vehicle/state_store.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

constexpr auto kTaskId = "123e4567-e89b-12d3-a456-426614174010";
constexpr auto kFirstEventId = "123e4567-e89b-12d3-a456-426614174020";
constexpr auto kSecondEventId = "123e4567-e89b-12d3-a456-426614174021";
constexpr auto kTerminalEventId = "123e4567-e89b-12d3-a456-426614174022";
constexpr auto kLease = "opaque-state-test-lease";
constexpr std::int64_t kLeaseExpiry = 4102444800;
constexpr auto kEvidence = R"JSON({"receiver":"file","fsync":true})JSON";

#ifndef _WIN32
void expect_mode(const std::filesystem::path& path, mode_t expected) {
    struct stat metadata {};
    EXPECT(::stat(path.c_str(), &metadata) == 0);
    if (::stat(path.c_str(), &metadata) == 0) {
        EXPECT((metadata.st_mode & 0777) == expected);
    }
}
#endif

void test_crash_safe_reservation() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-state-store-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto database = root / "state.sqlite3";

    {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(opened);
        if (!opened) {
            return;
        }
        auto store = std::move(opened.value());
#ifndef _WIN32
        expect_mode(root, 0700);
        expect_mode(database, 0600);
        expect_mode(database.string() + "-wal", 0600);
        expect_mode(database.string() + "-shm", 0600);
#endif
        EXPECT(store->allocate_sequence().value() == 1);
        EXPECT(store->allocate_sequence().value() == 2);
        EXPECT(store->reserved_through() == 4);
    }

    {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(opened);
        auto store = std::move(opened.value());
        EXPECT(store->allocate_sequence().value() == 5);
        EXPECT(store->align_server_sequence(10));
        EXPECT(store->allocate_sequence().value() == 11);
        EXPECT(store->reserved_through() == 14);
    }

    {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(opened);
        auto store = std::move(opened.value());
        EXPECT(store->allocate_sequence().value() == 15);
    }

    std::filesystem::remove_all(root, ignored);
}

void test_task_generation_and_status_outbox() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-status-outbox-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    const auto database = root / "state.sqlite3";
    const roc::vehicle::TaskGenerationKey first_generation{kTaskId, 1};
    const roc::vehicle::TaskGenerationKey second_generation{kTaskId, 2};

    {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(opened);
        if (!opened) {
            return;
        }
        auto store = std::move(opened.value());

        const auto registered = store->register_task_generation(first_generation, 20);
        EXPECT(registered);
        if (registered) {
            EXPECT(registered.value().state == roc::vehicle::TaskExecutionState::accepted);
            EXPECT(registered.value().progress_floor == 20);
            EXPECT(registered.value().progress == 20);
        }

        const auto lower_floor = store->register_task_generation(first_generation, 10);
        EXPECT(lower_floor);
        if (lower_floor) {
            EXPECT(lower_floor.value().progress_floor == 20);
            EXPECT(lower_floor.value().progress == 20);
        }

        const auto next_attempt = store->register_task_generation(second_generation, 0);
        EXPECT(next_attempt);
        if (next_attempt) {
            EXPECT(next_attempt.value().key.attempt == 2);
            EXPECT(next_attempt.value().progress == 0);
        }

        roc::vehicle::StatusEventRecord below_floor{
            kFirstEventId, first_generation,
            roc::vehicle::TaskExecutionState::downloading, 19, {}, {},
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174020","state":"downloading","progress":19})JSON",
            {}, kLease, kLeaseExpiry, kEvidence};
        EXPECT(!store->persist_status_event(below_floor));

        roc::vehicle::StatusEventRecord downloading{
            kFirstEventId, first_generation,
            roc::vehicle::TaskExecutionState::downloading, 20, {}, {},
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174020","state":"downloading","progress":20})JSON",
            {}, kLease, kLeaseExpiry, kEvidence};
        EXPECT(store->persist_status_event(downloading));
        EXPECT(store->persist_status_event(downloading));

        auto incorrectly_acknowledged = downloading;
        incorrectly_acknowledged.event_id =
            "123e4567-e89b-12d3-a456-426614174023";
        incorrectly_acknowledged.disposition =
            roc::vehicle::StatusEventDisposition::acknowledged;
        EXPECT(!store->persist_status_event(incorrectly_acknowledged));

        auto reused_event = downloading;
        reused_event.progress = 21;
        reused_event.payload_json =
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174020","state":"downloading","progress":21})JSON";
        EXPECT(!store->persist_status_event(reused_event));

        roc::vehicle::StatusEventRecord regressed{
            kSecondEventId, first_generation,
            roc::vehicle::TaskExecutionState::downloading, 19, {}, {},
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174021","state":"downloading","progress":19})JSON",
            {}, kLease, kLeaseExpiry, kEvidence};
        EXPECT(!store->persist_status_event(regressed));

        auto pending = store->pending_status_events();
        EXPECT(pending);
        if (pending) {
            EXPECT(pending.value().size() == 1);
            if (!pending.value().empty()) {
                EXPECT(pending.value().front().event_id == kFirstEventId);
                EXPECT(pending.value().front().payload_json == downloading.payload_json);
                EXPECT(pending.value().front().request_sha256.size() == 64);
                EXPECT(pending.value().front().lease_token == kLease);
                EXPECT(pending.value().front().lease_expires_at == kLeaseExpiry);
                EXPECT(pending.value().front().local_delivery_evidence == kEvidence);
            }
        }

        EXPECT(store->acknowledge_status_event(kFirstEventId));
        const auto acknowledged = store->status_event(kFirstEventId);
        EXPECT(acknowledged);
        if (acknowledged) {
            EXPECT(acknowledged.value().disposition ==
                   roc::vehicle::StatusEventDisposition::acknowledged);
            EXPECT(acknowledged.value().lease_token.empty());
            EXPECT(acknowledged.value().local_delivery_evidence == kEvidence);
        }
        pending = store->pending_status_events();
        EXPECT(pending);
        if (pending) {
            EXPECT(pending.value().empty());
        }

        roc::vehicle::StatusEventRecord delivering{
            kSecondEventId, first_generation,
            roc::vehicle::TaskExecutionState::delivering, 80, {}, {},
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174021","state":"delivering","progress":80})JSON",
            {}, kLease, kLeaseExpiry, kEvidence};
        EXPECT(store->persist_status_event(delivering));

        roc::vehicle::StatusEventRecord incomplete_delivery{
            kTerminalEventId, first_generation,
            roc::vehicle::TaskExecutionState::delivered, 99, {}, {},
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174022","state":"delivered","progress":99})JSON",
            {}, kLease, kLeaseExpiry, kEvidence};
        EXPECT(!store->persist_status_event(incomplete_delivery));

        roc::vehicle::StatusEventRecord delivered{
            kTerminalEventId, first_generation,
            roc::vehicle::TaskExecutionState::delivered, 100, {}, {},
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174022","state":"delivered","progress":100})JSON",
            {}, kLease, kLeaseExpiry, kEvidence};
        EXPECT(store->persist_status_event(delivered));
        EXPECT(store->persist_status_event(delivered));
        const auto blocked = store->has_unconfirmed_terminal_event(kTaskId);
        EXPECT(blocked);
        if (blocked) {
            EXPECT(blocked.value());
        }
    }

    {
        auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(reopened);
        if (!reopened) {
            return;
        }
        auto store = std::move(reopened.value());
        const auto generation = store->task_generation(first_generation);
        EXPECT(generation);
        if (generation) {
            EXPECT(generation.value().state == roc::vehicle::TaskExecutionState::delivered);
            EXPECT(generation.value().progress == 100);
        }

        const auto pending = store->pending_status_events();
        EXPECT(pending);
        if (pending) {
            EXPECT(pending.value().size() == 2);
            if (pending.value().size() == 2) {
                EXPECT(pending.value()[0].event_id == kSecondEventId);
                EXPECT(pending.value()[1].event_id == kTerminalEventId);
            }
        }

        roc::vehicle::StatusEventRecord delivered{
            kTerminalEventId, first_generation,
            roc::vehicle::TaskExecutionState::delivered, 100, {}, {},
            R"JSON({"event_id":"123e4567-e89b-12d3-a456-426614174022","state":"delivered","progress":100})JSON",
            {}, kLease, kLeaseExpiry, kEvidence};
        EXPECT(store->persist_status_event(delivered));
        EXPECT(store->acknowledge_status_event(kTerminalEventId));
        const auto blocked = store->has_unconfirmed_terminal_event(kTaskId);
        EXPECT(blocked);
        if (blocked) {
            EXPECT(!blocked.value());
        }
    }

    std::filesystem::remove_all(root, ignored);
}

void test_v1_outbox_migration(std::string state,
                              int progress,
                              roc::vehicle::TaskExecutionState expected_generation,
                              roc::vehicle::StatusEventDisposition expected_disposition,
                              bool expected_reconciliation) {
    const auto root = std::filesystem::temp_directory_path() /
                      ("roc-vehicle-v1-migration-" + state);
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
#ifndef _WIN32
    EXPECT(::chmod(root.c_str(), 0700) == 0);
#endif
    const auto database = root / "state.sqlite3";
    sqlite3* raw_database = nullptr;
    EXPECT(sqlite3_open(database.string().c_str(), &raw_database) == SQLITE_OK);
    if (raw_database == nullptr) {
        return;
    }
    const std::string error_fields = state == "failed"
        ? "'legacy_failure', 'legacy failure'"
        : "NULL, NULL";
    const std::string v1_schema = R"SQL(
CREATE TABLE schema_migrations(version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL);
INSERT INTO schema_migrations VALUES(1, 'test');
CREATE TABLE sequence_state(singleton INTEGER PRIMARY KEY, reserved_through INTEGER NOT NULL);
INSERT INTO sequence_state VALUES(1, 0);
CREATE TABLE task_generations(
  task_id TEXT NOT NULL, attempt INTEGER NOT NULL, state TEXT NOT NULL,
  progress_floor INTEGER NOT NULL, progress INTEGER NOT NULL, updated_at TEXT NOT NULL,
  PRIMARY KEY(task_id, attempt));
CREATE TABLE status_events(
  event_id TEXT PRIMARY KEY, task_id TEXT NOT NULL, attempt INTEGER NOT NULL,
  state TEXT NOT NULL, progress INTEGER NOT NULL, error_code TEXT,
  error_message TEXT, payload_json TEXT NOT NULL,
  acknowledged INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL);
INSERT INTO task_generations VALUES(
  '123e4567-e89b-12d3-a456-426614174010', 1, ')SQL" + state +
  "', 0, " + std::to_string(progress) + R"SQL(, 'test');
INSERT INTO status_events VALUES(
  '123e4567-e89b-12d3-a456-426614174025',
  '123e4567-e89b-12d3-a456-426614174010', 1, ')SQL" + state + "', " +
  std::to_string(progress) + ", " + error_fields +
  R"SQL(, '{"event_id":"legacy"}', 0, 'test');
)SQL";
    char* sqlite_error = nullptr;
    EXPECT(sqlite3_exec(raw_database, v1_schema.c_str(), nullptr, nullptr, &sqlite_error) ==
           SQLITE_OK);
    if (sqlite_error != nullptr) {
        sqlite3_free(sqlite_error);
    }
    EXPECT(sqlite3_close(raw_database) == SQLITE_OK);
#ifndef _WIN32
    // Raw SQLite fixture creation uses the test process's ordinary umask.
    // Runtime refuses broad existing DBs rather than opening/closing an extra
    // descriptor to repair permissions and thereby releasing SQLite locks.
    expect_mode(database,0644);
    EXPECT(!roc::vehicle::DurableStateStore::open(database,4));
    expect_mode(database,0644);
    EXPECT(::chmod(database.c_str(),0600)==0); // Offline fixture provisioning.
#endif

    auto opened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(opened);
    if (opened) {
        const auto reconciliations = opened.value()->reconciliation_required_events();
        EXPECT(reconciliations);
        if (reconciliations) {
            EXPECT(reconciliations.value().size() ==
                   (expected_reconciliation ? 1U : 0U));
        }
        const auto event = opened.value()->status_event(
            "123e4567-e89b-12d3-a456-426614174025");
        EXPECT(event);
        if (event) {
            EXPECT(event.value().disposition == expected_disposition);
            EXPECT(event.value().lease_token.empty());
            EXPECT(event.value().reconciliation_reason ==
                   (expected_reconciliation
                        ? "legacy_terminal_missing_replay_material"
                        : "legacy_nonterminal_missing_replay_material"));
        }
        const auto generation = opened.value()->task_generation({kTaskId, 1});
        EXPECT(generation);
        if (generation) {
            EXPECT(generation.value().state == expected_generation);
        }
        const auto pending = opened.value()->pending_status_events();
        EXPECT(pending);
        if (pending) {
            EXPECT(pending.value().empty());
        }
    }
    std::filesystem::remove_all(root, ignored);
}

void test_shared_parent_permissions_are_not_modified() {
#ifndef _WIN32
    const auto shared = std::filesystem::temp_directory_path() /
                        "roc-vehicle-shared-parent-test";
    std::error_code ignored;
    std::filesystem::remove_all(shared, ignored);
    std::filesystem::create_directories(shared, ignored);
    EXPECT(::chmod(shared.c_str(), 0755) == 0);

    const auto rejected = roc::vehicle::DurableStateStore::open(
        shared / "unsafe.sqlite3", 4);
    EXPECT(!rejected);
    expect_mode(shared, 0755);
    EXPECT(!std::filesystem::exists(shared / "unsafe.sqlite3"));

    const auto dedicated = shared / "vehicle-state";
    {
        auto opened = roc::vehicle::DurableStateStore::open(
            dedicated / "state.sqlite3", 4);
        EXPECT(opened);
        if (opened) {
            expect_mode(shared, 0755);
            expect_mode(dedicated, 0700);
            expect_mode(dedicated / "state.sqlite3", 0600);
            expect_mode(dedicated / "state.sqlite3-wal", 0600);
            expect_mode(dedicated / "state.sqlite3-shm", 0600);
        }
    }
    expect_mode(shared, 0755);

    struct stat temporary_before {};
    struct stat temporary_after {};
    const auto temporary = std::filesystem::temp_directory_path();
    EXPECT(::stat(temporary.c_str(), &temporary_before) == 0);
    const auto direct_temporary = roc::vehicle::DurableStateStore::open(
        temporary / "roc-vehicle-unsafe-root.sqlite3", 4);
    EXPECT(!direct_temporary);
    EXPECT(::stat(temporary.c_str(), &temporary_after) == 0);
    EXPECT((temporary_before.st_mode & 07777) ==
           (temporary_after.st_mode & 07777));
    EXPECT(!std::filesystem::exists(
        temporary / "roc-vehicle-unsafe-root.sqlite3"));
    std::filesystem::remove_all(shared, ignored);
#endif
}

void test_corrupt_database_is_rejected() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-corrupt-database-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root, ignored);
#ifndef _WIN32
    EXPECT(::chmod(root.c_str(), 0700) == 0);
#endif
    const auto database = root / "state.sqlite3";
    {
        std::ofstream output{database, std::ios::binary};
        output << "not a sqlite database";
    }
    const auto opened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(!opened);
#ifndef _WIN32
    expect_mode(root, 0700);
    expect_mode(database, 0644); // Rejection never claims or chmods this input.
#endif
    std::filesystem::remove_all(root, ignored);
}

}  // namespace

int main() {
    test_crash_safe_reservation();
    test_task_generation_and_status_outbox();
    test_v1_outbox_migration(
        "downloading", 20, roc::vehicle::TaskExecutionState::expired,
        roc::vehicle::StatusEventDisposition::forensic, false);
    test_v1_outbox_migration(
        "delivering", 80, roc::vehicle::TaskExecutionState::expired,
        roc::vehicle::StatusEventDisposition::forensic, false);
    test_v1_outbox_migration(
        "delivered", 100,
        roc::vehicle::TaskExecutionState::reconciliation_required,
        roc::vehicle::StatusEventDisposition::reconciliation_required, true);
    test_v1_outbox_migration(
        "failed", 80,
        roc::vehicle::TaskExecutionState::reconciliation_required,
        roc::vehicle::StatusEventDisposition::reconciliation_required, true);
    test_shared_parent_permissions_are_not_modified();
    test_corrupt_database_is_rejected();
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
