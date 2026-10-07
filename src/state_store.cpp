#include "roc/vehicle/state_store.hpp"

#include "roc/vehicle/crypto.hpp"
#include "private_paths.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cerrno>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace roc::vehicle {
namespace {

Error storage_error(std::string message) {
    return Error{ErrorDomain::storage, ErrorCode::io_failure, false, std::move(message)};
}

Error state_error(std::string message) {
    return Error{ErrorDomain::storage, ErrorCode::invalid_state, false, std::move(message)};
}

std::string_view state_name(TaskExecutionState state) {
    switch (state) {
        case TaskExecutionState::accepted: return "accepted";
        case TaskExecutionState::downloading: return "downloading";
        case TaskExecutionState::delivering: return "delivering";
        case TaskExecutionState::delivered: return "delivered";
        case TaskExecutionState::failed: return "failed";
        case TaskExecutionState::reconciliation_required: return "reconciliation_required";
        case TaskExecutionState::expired: return "expired";
        case TaskExecutionState::canceled: return "canceled";
    }
    return "invalid";
}

bool parse_state(std::string_view text, TaskExecutionState& state) {
    if (text == "accepted") state = TaskExecutionState::accepted;
    else if (text == "downloading") state = TaskExecutionState::downloading;
    else if (text == "delivering") state = TaskExecutionState::delivering;
    else if (text == "delivered") state = TaskExecutionState::delivered;
    else if (text == "failed") state = TaskExecutionState::failed;
    else if (text == "reconciliation_required") {
        state = TaskExecutionState::reconciliation_required;
    }
    else if (text == "expired") state = TaskExecutionState::expired;
    else if (text == "canceled") state = TaskExecutionState::canceled;
    else return false;
    return true;
}

std::string_view disposition_name(StatusEventDisposition disposition) {
    switch (disposition) {
        case StatusEventDisposition::pending: return "pending";
        case StatusEventDisposition::acknowledged: return "acknowledged";
        case StatusEventDisposition::reconciliation_required:
            return "reconciliation_required";
        case StatusEventDisposition::resolved: return "resolved";
        case StatusEventDisposition::forensic: return "forensic";
    }
    return "invalid";
}

bool parse_disposition(std::string_view text, StatusEventDisposition& disposition) {
    if (text == "pending") disposition = StatusEventDisposition::pending;
    else if (text == "acknowledged") {
        disposition = StatusEventDisposition::acknowledged;
    } else if (text == "reconciliation_required") {
        disposition = StatusEventDisposition::reconciliation_required;
    } else if (text == "resolved") {
        disposition = StatusEventDisposition::resolved;
    } else if (text == "forensic") {
        disposition = StatusEventDisposition::forensic;
    } else {
        return false;
    }
    return true;
}

bool is_reportable_state(TaskExecutionState state) {
    return state == TaskExecutionState::downloading ||
           state == TaskExecutionState::delivering ||
           state == TaskExecutionState::delivered ||
           state == TaskExecutionState::failed;
}

bool valid_transition(TaskExecutionState current, TaskExecutionState next) {
    if (current == next) {
        return next == TaskExecutionState::downloading ||
               next == TaskExecutionState::delivering;
    }
    if (next == TaskExecutionState::failed) {
        return current == TaskExecutionState::accepted ||
               current == TaskExecutionState::downloading ||
               current == TaskExecutionState::delivering;
    }
    if (current == TaskExecutionState::accepted) {
        return next == TaskExecutionState::downloading;
    }
    if (current == TaskExecutionState::downloading) {
        return next == TaskExecutionState::delivering;
    }
    if (current == TaskExecutionState::delivering) {
        return next == TaskExecutionState::delivered;
    }
    return false;
}

bool bind_text(sqlite3_stmt* statement, int index, std::string_view value) {
    return sqlite3_bind_text(statement, index, value.data(),
                             static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK;
}

bool execute(sqlite3* database, const char* sql) {
    char* error = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &error);
    if (error != nullptr) {
        sqlite3_free(error);
    }
    return result == SQLITE_OK;
}

#ifndef _WIN32
bool secure_regular_file(const std::filesystem::path& path, bool missing_is_ok) {
    // Do not open/close another descriptor for a file managed by SQLite.
    // POSIX record locks are process-scoped: closing ANY descriptor for the
    // DB or SHM inode can release SQLite's locks on its other connections.
    // SQLite inherits WAL/SHM modes from the exclusively-created 0600 DB;
    // validate those modes without touching descriptors or process umask.
    struct stat data {};
    if (::lstat(path.c_str(), &data) != 0) {
        return missing_is_ok && errno == ENOENT;
    }
    return S_ISREG(data.st_mode) && data.st_uid == ::geteuid() &&
           data.st_nlink == 1 && (data.st_mode & 07777) == 0600;
}

bool create_private_database_file(const std::filesystem::path& path) {
    const auto native = path.u8string();
    const int descriptor = ::open(native.c_str(),
                                  O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                                  0600);
    if (descriptor >= 0) {
        return ::close(descriptor) == 0 && secure_regular_file(path, false);
    }
    return errno == EEXIST && secure_regular_file(path, false);
}

bool harden_state_paths(const std::filesystem::path& database_path) {
    const auto parent = database_path.parent_path();
    if (!detail::private_directory(parent,false) ||
        !secure_regular_file(database_path, false)) {
        return false;
    }
    auto wal = database_path;
    wal += "-wal";
    auto shm = database_path;
    shm += "-shm";
    return secure_regular_file(wal, true) && secure_regular_file(shm, true);
}
#else
bool create_private_database_file(const std::filesystem::path&) { return true; }
bool harden_state_paths(const std::filesystem::path&) { return true; }
#endif

bool prohibited_state_directory(const std::filesystem::path& directory) {
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(directory, error);
    if (error || normalized.empty() || normalized == normalized.root_path()) {
        return true;
    }
    const auto temporary = std::filesystem::weakly_canonical(
        std::filesystem::temp_directory_path(error), error);
    if (error || normalized == temporary) {
        return true;
    }
#ifndef _WIN32
    if (normalized == std::filesystem::path{"/var/tmp"}) {
        return true;
    }
#endif
    return false;
}

bool prepare_private_state_directory(const std::filesystem::path& directory) {
    if (directory.empty() || prohibited_state_directory(directory)) {
        return false;
    }
#ifndef _WIN32
    return detail::private_directory(directory,true);
#else
    std::error_code error;
    const auto status = std::filesystem::symlink_status(directory, error);
    if (!error && std::filesystem::exists(status)) {
        if (!std::filesystem::is_directory(status) ||
            std::filesystem::is_symlink(status)) {
            return false;
        }
        return true;
    }
    if (error && error != std::errc::no_such_file_or_directory) {
        return false;
    }
    const auto ancestor = directory.parent_path();
    if (ancestor.empty()) {
        return false;
    }
    std::filesystem::create_directories(ancestor, error);
    if (error) {
        return false;
    }
    return std::filesystem::create_directory(directory, error) ||
           (!error && std::filesystem::is_directory(directory));
#endif
}

class Statement {
public:
    Statement(sqlite3* database, const char* sql) {
        if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
            statement_ = nullptr;
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    ~Statement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }
    [[nodiscard]] explicit operator bool() const noexcept { return statement_ != nullptr; }

private:
    sqlite3_stmt* statement_{nullptr};
};

bool initialize_database(sqlite3* database) {
    if (!execute(database, "PRAGMA journal_mode=WAL") ||
        !execute(database, "PRAGMA synchronous=FULL") ||
        !execute(database, "PRAGMA foreign_keys=ON") ||
        !execute(database, R"SQL(
CREATE TABLE IF NOT EXISTS schema_migrations (
    version INTEGER PRIMARY KEY,
    applied_at TEXT NOT NULL
)
)SQL")) {
        return false;
    }

    Statement version_query{database,
                            "SELECT COALESCE(MAX(version), 0) FROM schema_migrations"};
    if (!version_query || sqlite3_step(version_query.get()) != SQLITE_ROW) {
        return false;
    }
    auto version = sqlite3_column_int64(version_query.get(), 0);
    constexpr std::int64_t latest_version = 3;
    if (version < 0 || version > latest_version) {
        return false;
    }

    if (version == 0) {
        if (!execute(database, "BEGIN IMMEDIATE")) {
            return false;
        }
        constexpr const char* migration_1 = R"SQL(
CREATE TABLE sequence_state (
    singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
    reserved_through INTEGER NOT NULL CHECK(reserved_through >= 0)
);
INSERT INTO sequence_state(singleton, reserved_through) VALUES(1, 0);
CREATE TABLE task_generations (
    task_id TEXT NOT NULL,
    attempt INTEGER NOT NULL CHECK(attempt >= 0),
    state TEXT NOT NULL,
    progress_floor INTEGER NOT NULL CHECK(progress_floor BETWEEN 0 AND 100),
    progress INTEGER NOT NULL CHECK(progress BETWEEN 0 AND 100),
    updated_at TEXT NOT NULL,
    PRIMARY KEY(task_id, attempt)
);
CREATE TABLE status_events (
    event_id TEXT PRIMARY KEY,
    task_id TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    state TEXT NOT NULL,
    progress INTEGER NOT NULL CHECK(progress BETWEEN 0 AND 100),
    error_code TEXT,
    error_message TEXT,
    payload_json TEXT NOT NULL,
    acknowledged INTEGER NOT NULL DEFAULT 0 CHECK(acknowledged IN (0, 1)),
    created_at TEXT NOT NULL,
    FOREIGN KEY(task_id, attempt) REFERENCES task_generations(task_id, attempt)
);
INSERT INTO schema_migrations(version, applied_at)
VALUES(1, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));
)SQL";
        if (!execute(database, migration_1) || !execute(database, "COMMIT")) {
            execute(database, "ROLLBACK");
            return false;
        }
        version = 1;
    }

    if (version == 1) {
        if (!execute(database, "BEGIN IMMEDIATE")) {
            return false;
        }
        constexpr const char* migration_2 = R"SQL(
ALTER TABLE status_events ADD COLUMN request_sha256 TEXT;
ALTER TABLE status_events ADD COLUMN lease_token TEXT;
ALTER TABLE status_events ADD COLUMN lease_expires_at INTEGER;
ALTER TABLE status_events ADD COLUMN local_delivery_evidence TEXT;
ALTER TABLE status_events ADD COLUMN disposition TEXT NOT NULL DEFAULT 'pending';
ALTER TABLE status_events ADD COLUMN reconciliation_reason TEXT;
UPDATE status_events
SET disposition = CASE
    WHEN acknowledged = 1 THEN 'acknowledged'
    WHEN state IN ('delivered', 'failed') THEN 'reconciliation_required'
    ELSE 'forensic'
END,
reconciliation_reason = CASE
    WHEN acknowledged = 0 AND state IN ('delivered', 'failed')
        THEN 'legacy_terminal_missing_replay_material'
    WHEN acknowledged = 0 THEN 'legacy_nonterminal_missing_replay_material'
    ELSE NULL
END;
UPDATE task_generations
SET state = 'reconciliation_required',
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
WHERE EXISTS (
    SELECT 1 FROM status_events e
    WHERE e.task_id = task_generations.task_id
      AND e.attempt = task_generations.attempt
      AND e.state IN ('delivered', 'failed')
      AND e.disposition = 'reconciliation_required'
);
UPDATE task_generations
SET state = 'expired',
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
WHERE EXISTS (
    SELECT 1 FROM status_events e
    WHERE e.task_id = task_generations.task_id
      AND e.attempt = task_generations.attempt
      AND e.state IN ('downloading', 'delivering')
      AND e.disposition = 'forensic'
)
AND NOT EXISTS (
    SELECT 1 FROM status_events terminal
    WHERE terminal.task_id = task_generations.task_id
      AND terminal.attempt = task_generations.attempt
      AND terminal.state IN ('delivered', 'failed')
      AND terminal.disposition = 'reconciliation_required'
);
INSERT INTO schema_migrations(version, applied_at)
VALUES(2, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));
)SQL";
        if (!execute(database, migration_2) || !execute(database, "COMMIT")) {
            execute(database, "ROLLBACK");
            return false;
        }
        version = 2;
    }

    if (version == 2) {
        if (!execute(database, R"SQL(
BEGIN IMMEDIATE;
CREATE TABLE active_task_leases (
    task_id TEXT NOT NULL,
    attempt INTEGER NOT NULL,
    batch_id TEXT NOT NULL,
    project_id TEXT NOT NULL,
    resource_type TEXT NOT NULL,
    resource_revision_id TEXT NOT NULL,
    max_attempts INTEGER NOT NULL,
    lease_token TEXT NOT NULL,
    lease_expires_at INTEGER NOT NULL,
    PRIMARY KEY(task_id, attempt),
    FOREIGN KEY(task_id, attempt) REFERENCES task_generations(task_id, attempt)
);
CREATE TRIGGER clear_active_terminal_lease AFTER UPDATE OF disposition ON status_events
WHEN NEW.state IN ('delivered', 'failed') AND NEW.disposition IN ('acknowledged', 'resolved')
BEGIN
    DELETE FROM active_task_leases WHERE task_id = NEW.task_id AND attempt = NEW.attempt;
END;
CREATE TRIGGER clear_expired_active_lease AFTER UPDATE OF state ON task_generations
WHEN NEW.state IN ('expired', 'canceled')
BEGIN
    DELETE FROM active_task_leases WHERE task_id = NEW.task_id AND attempt = NEW.attempt;
END;
INSERT INTO schema_migrations(version, applied_at)
VALUES(3, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'));
COMMIT;
)SQL")) {
            execute(database, "ROLLBACK");
            return false;
        }
    }

    Statement validate_sequence{
        database, "SELECT reserved_through FROM sequence_state WHERE singleton = 1"};
    Statement validate_generation{
        database, "SELECT task_id, attempt, state, progress_floor, progress "
                  "FROM task_generations LIMIT 0"};
    Statement validate_outbox{
        database, "SELECT event_id, task_id, attempt, state, progress, payload_json, "
                  "request_sha256, lease_token, lease_expires_at, "
                  "local_delivery_evidence, disposition, reconciliation_reason "
                  "FROM status_events LIMIT 0"};
    Statement validate_active_lease{
        database, "SELECT task_id, attempt, batch_id, project_id, resource_type, "
                  "resource_revision_id, max_attempts, lease_token, lease_expires_at "
                  "FROM active_task_leases LIMIT 0"};
    return static_cast<bool>(validate_sequence) &&
           static_cast<bool>(validate_generation) &&
           static_cast<bool>(validate_outbox) &&
           static_cast<bool>(validate_active_lease);
}

std::string column_text(sqlite3_stmt* statement, int column) {
    const auto* value = reinterpret_cast<const char*>(
        sqlite3_column_text(statement, column));
    return value == nullptr ? std::string{} : std::string{value};
}

Result<StatusEventRecord> read_status_event_row(sqlite3_stmt* statement) {
    TaskExecutionState state{};
    StatusEventDisposition disposition{};
    if (!parse_state(column_text(statement, 3), state) ||
        !parse_disposition(column_text(statement, 12), disposition)) {
        return storage_error("stored status event state is invalid");
    }
    return StatusEventRecord{
        column_text(statement, 0),
        {column_text(statement, 1), sqlite3_column_int64(statement, 2)},
        state,
        sqlite3_column_int64(statement, 4),
        column_text(statement, 5),
        column_text(statement, 6),
        column_text(statement, 7),
        column_text(statement, 8),
        column_text(statement, 9),
        sqlite3_column_int64(statement, 10),
        column_text(statement, 11),
        disposition,
        column_text(statement, 13)};
}

}  // namespace

struct DurableStateStore::Impl {
    sqlite3* database{nullptr};
    std::filesystem::path database_path;
    std::uint32_t reservation_size{64};
    std::uint64_t next_sequence{1};
    std::int64_t reserved{0};
    std::int64_t server_floor{0};
    mutable std::mutex mutex;

    [[nodiscard]] bool harden() const {
        return harden_state_paths(database_path);
    }

    ~Impl() {
        if (database != nullptr) {
            sqlite3_close(database);
        }
    }
};

DurableStateStore::DurableStateStore(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

DurableStateStore::~DurableStateStore() = default;

Result<std::unique_ptr<DurableStateStore>> DurableStateStore::open(
    const std::filesystem::path& database_path,
    std::uint32_t sequence_reservation_size) {
    if (database_path.empty() || sequence_reservation_size == 0) {
        return storage_error("state database path and sequence reservation size are required");
    }

    std::error_code filesystem_error;
    auto absolute_path = std::filesystem::absolute(database_path, filesystem_error)
                             .lexically_normal();
    if (filesystem_error || absolute_path.filename().empty() ||
        absolute_path.filename() == "." || absolute_path.filename() == "..") {
        return storage_error("state database path is invalid");
    }
    const auto requested_parent = absolute_path.parent_path();
    if (!prepare_private_state_directory(requested_parent)) {
        return storage_error("state directory is not a private Vehicle directory");
    }
    const auto managed_parent =
        std::filesystem::weakly_canonical(requested_parent, filesystem_error);
    if (filesystem_error || prohibited_state_directory(managed_parent)) {
        return storage_error("state directory is not safe for Vehicle state");
    }
    const auto managed_database_path = managed_parent / absolute_path.filename();
    if (!create_private_database_file(managed_database_path)) {
        return storage_error("unable to create a private state database file");
    }
    if (!harden_state_paths(managed_database_path)) {
        return storage_error("state database files are not private regular files");
    }

    auto impl = std::make_unique<Impl>();
    impl->database_path = managed_database_path;
    impl->reservation_size = sequence_reservation_size;
    const auto path = managed_database_path.u8string();
    if (sqlite3_open_v2(path.c_str(), &impl->database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                            SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
        return storage_error("unable to open state database");
    }
    sqlite3_busy_timeout(impl->database, 5000);

    if (!initialize_database(impl->database)) {
        return storage_error("unable to initialize state database");
    }
    if (!impl->harden()) {
        return storage_error("unable to apply private state database permissions");
    }

    Statement select{impl->database,
                     "SELECT reserved_through FROM sequence_state WHERE singleton = 1"};
    if (!select || sqlite3_step(select.get()) != SQLITE_ROW) {
        return storage_error("unable to read sequence state");
    }
    impl->reserved = sqlite3_column_int64(select.get(), 0);
    impl->next_sequence = static_cast<std::uint64_t>(impl->reserved) + 1U;

    return std::unique_ptr<DurableStateStore>{
        new DurableStateStore{std::move(impl)}};
}

Result<void> DurableStateStore::align_server_sequence(
    std::int64_t last_client_sequence) {
    if (last_client_sequence < 0) {
        return storage_error("server sequence cursor must be nonnegative");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    impl_->server_floor = std::max(impl_->server_floor, last_client_sequence);
    const auto candidate = static_cast<std::uint64_t>(last_client_sequence) + 1U;
    impl_->next_sequence = std::max(impl_->next_sequence, candidate);
    return {};
}

Result<Sequence> DurableStateStore::allocate_sequence() {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    constexpr auto maximum = static_cast<std::uint64_t>(
        std::numeric_limits<std::int64_t>::max());
    if (impl_->next_sequence > maximum) {
        return storage_error("positive INT64 sequence space is exhausted");
    }

    if (impl_->next_sequence > static_cast<std::uint64_t>(impl_->reserved)) {
        if (!execute(impl_->database, "BEGIN IMMEDIATE")) {
            return storage_error("unable to begin sequence reservation transaction");
        }

        Statement select{impl_->database,
                         "SELECT reserved_through FROM sequence_state WHERE singleton = 1"};
        if (!select || sqlite3_step(select.get()) != SQLITE_ROW) {
            execute(impl_->database, "ROLLBACK");
            return storage_error("unable to read sequence reservation");
        }
        const auto database_reserved = sqlite3_column_int64(select.get(), 0);
        const auto base = std::max({static_cast<std::uint64_t>(database_reserved),
                                    static_cast<std::uint64_t>(impl_->server_floor),
                                    impl_->next_sequence - 1U});
        if (base >= maximum) {
            execute(impl_->database, "ROLLBACK");
            return storage_error("positive INT64 sequence space is exhausted");
        }
        const auto available = maximum - base;
        const auto block = std::min<std::uint64_t>(impl_->reservation_size, available);
        const auto new_reserved = base + block;

        Statement update{impl_->database,
                         "UPDATE sequence_state SET reserved_through = ? WHERE singleton = 1"};
        if (!update || sqlite3_bind_int64(update.get(), 1,
                                          static_cast<sqlite3_int64>(new_reserved)) != SQLITE_OK ||
            sqlite3_step(update.get()) != SQLITE_DONE ||
            !execute(impl_->database, "COMMIT")) {
            execute(impl_->database, "ROLLBACK");
            return storage_error("unable to persist sequence reservation");
        }
        impl_->reserved = static_cast<std::int64_t>(new_reserved);
        impl_->next_sequence = base + 1U;
    }

    const auto allocated = impl_->next_sequence;
    ++impl_->next_sequence;
    return static_cast<Sequence>(allocated);
}

std::int64_t DurableStateStore::reserved_through() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    return impl_->reserved;
}

Result<TaskGenerationState> DurableStateStore::register_task_generation(
    const TaskGenerationKey& key,
    std::int64_t accepted_progress_floor) {
    if (!is_valid_uuid(key.task_id) || key.attempt < 1 ||
        accepted_progress_floor < 0 || accepted_progress_floor > 100) {
        return state_error("task generation key or progress floor is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    Statement insert{impl_->database, R"SQL(
INSERT INTO task_generations(task_id, attempt, state, progress_floor, progress, updated_at)
VALUES(?, ?, 'accepted', ?, ?, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
ON CONFLICT(task_id, attempt) DO UPDATE SET
    progress_floor = MAX(progress_floor, excluded.progress_floor),
    progress = MAX(progress, excluded.progress_floor),
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
)SQL"};
    if (!insert || !bind_text(insert.get(), 1, key.task_id) ||
        sqlite3_bind_int64(insert.get(), 2, key.attempt) != SQLITE_OK ||
        sqlite3_bind_int64(insert.get(), 3, accepted_progress_floor) != SQLITE_OK ||
        sqlite3_bind_int64(insert.get(), 4, accepted_progress_floor) != SQLITE_OK ||
        sqlite3_step(insert.get()) != SQLITE_DONE) {
        return storage_error("unable to persist task generation");
    }

    Statement select{impl_->database, R"SQL(
SELECT state, progress_floor, progress FROM task_generations
WHERE task_id = ? AND attempt = ?
)SQL"};
    if (!select || !bind_text(select.get(), 1, key.task_id) ||
        sqlite3_bind_int64(select.get(), 2, key.attempt) != SQLITE_OK ||
        sqlite3_step(select.get()) != SQLITE_ROW) {
        return storage_error("unable to read task generation");
    }
    const auto* state_text = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 0));
    TaskExecutionState state{};
    if (state_text == nullptr || !parse_state(state_text, state)) {
        return storage_error("stored task generation state is invalid");
    }
    return TaskGenerationState{key, state, sqlite3_column_int64(select.get(), 1),
                               sqlite3_column_int64(select.get(), 2)};
}

Result<TaskGenerationState> DurableStateStore::task_generation(
    const TaskGenerationKey& key) const {
    if (!is_valid_uuid(key.task_id) || key.attempt < 1) {
        return state_error("task generation key is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT state, progress_floor, progress FROM task_generations
WHERE task_id = ? AND attempt = ?
)SQL"};
    if (!select || !bind_text(select.get(), 1, key.task_id) ||
        sqlite3_bind_int64(select.get(), 2, key.attempt) != SQLITE_OK) {
        return storage_error("unable to query task generation");
    }
    if (sqlite3_step(select.get()) != SQLITE_ROW) {
        return state_error("task generation does not exist");
    }
    const auto* state_text = reinterpret_cast<const char*>(sqlite3_column_text(select.get(), 0));
    TaskExecutionState state{};
    if (state_text == nullptr || !parse_state(state_text, state)) {
        return storage_error("stored task generation state is invalid");
    }
    return TaskGenerationState{key, state, sqlite3_column_int64(select.get(), 1),
                               sqlite3_column_int64(select.get(), 2)};
}

Result<void> DurableStateStore::mark_task_generation_expired(
    const TaskGenerationKey& key) {
    if (!is_valid_uuid(key.task_id) || key.attempt < 1) {
        return state_error("task generation key is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    Statement update{impl_->database, R"SQL(
UPDATE task_generations SET state = 'expired',
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
WHERE task_id = ? AND attempt = ?
  AND state IN ('accepted', 'downloading', 'delivering')
)SQL"};
    if (!update || !bind_text(update.get(), 1, key.task_id) ||
        sqlite3_bind_int64(update.get(), 2, key.attempt) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE) {
        return storage_error("unable to expire task generation");
    }
    if (sqlite3_changes(impl_->database) != 1) {
        return state_error("task generation cannot be expired");
    }
    return {};
}

Result<void> DurableStateStore::persist_active_task_lease(const ActiveTaskLease& lease) {
    const auto& n = lease.notification;
    const auto& key = lease.generation;
    if (n.task_id != key.task_id || !is_valid_uuid(key.task_id) || key.attempt < 1 ||
        !is_valid_uuid(n.batch_id) || !is_valid_uuid(n.project_id) ||
        !is_valid_uuid(n.resource_revision_id) || n.max_attempts < 1 || n.max_attempts > 10 ||
        lease.lease_token.size() < 16 || lease.lease_token.size() > 8192 ||
        lease.lease_expires_at < 1)
        return state_error("active lease material is invalid");
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    Statement insert{impl_->database, R"SQL(
INSERT INTO active_task_leases(task_id, attempt, batch_id, project_id, resource_type,
    resource_revision_id, max_attempts, lease_token, lease_expires_at)
SELECT ?, ?, ?, ?, ?, ?, ?, ?, ?
WHERE EXISTS(SELECT 1 FROM task_generations WHERE task_id = ? AND attempt = ?
    AND state IN ('accepted', 'downloading', 'delivering'))
ON CONFLICT(task_id, attempt) DO UPDATE SET lease_token = excluded.lease_token,
    lease_expires_at = excluded.lease_expires_at
WHERE active_task_leases.lease_token = excluded.lease_token
    AND active_task_leases.lease_expires_at = excluded.lease_expires_at
    AND active_task_leases.batch_id = excluded.batch_id
    AND active_task_leases.project_id = excluded.project_id
    AND active_task_leases.resource_type = excluded.resource_type
    AND active_task_leases.resource_revision_id = excluded.resource_revision_id
    AND active_task_leases.max_attempts = excluded.max_attempts
)SQL"};
    if (!insert || !bind_text(insert.get(), 1, key.task_id) ||
        sqlite3_bind_int64(insert.get(), 2, key.attempt) != SQLITE_OK ||
        !bind_text(insert.get(), 3, n.batch_id) || !bind_text(insert.get(), 4, n.project_id) ||
        !bind_text(insert.get(), 5, n.resource_type == ResourceType::map ? "map" : "road_network") ||
        !bind_text(insert.get(), 6, n.resource_revision_id) ||
        sqlite3_bind_int64(insert.get(), 7, n.max_attempts) != SQLITE_OK ||
        !bind_text(insert.get(), 8, lease.lease_token) ||
        sqlite3_bind_int64(insert.get(), 9, lease.lease_expires_at) != SQLITE_OK ||
        !bind_text(insert.get(), 10, key.task_id) ||
        sqlite3_bind_int64(insert.get(), 11, key.attempt) != SQLITE_OK ||
        sqlite3_step(insert.get()) != SQLITE_DONE || sqlite3_changes(impl_->database) != 1)
        return state_error("unable to persist active lease without replacing its identity");
    if (!impl_->harden()) return storage_error("active state permissions are invalid");
    return {};
}

Result<std::vector<TaskGenerationKey>> DurableStateStore::active_task_keys() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT a.task_id, a.attempt FROM active_task_leases a JOIN task_generations g
ON a.task_id = g.task_id AND a.attempt = g.attempt
WHERE g.state IN ('accepted','downloading','delivering') ORDER BY a.task_id, a.attempt
)SQL"};
    if (!select) return storage_error("unable to list active tasks");
    std::vector<TaskGenerationKey> keys;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(select.get())) == SQLITE_ROW)
        keys.push_back({column_text(select.get(), 0), sqlite3_column_int64(select.get(), 1)});
    if (step != SQLITE_DONE) return storage_error("unable to read active tasks");
    return keys;
}

Result<bool> DurableStateStore::has_unrecoverable_active_generation(
    std::string_view task_id) const {
    if (!is_valid_uuid(task_id)) return state_error("task identity is invalid");
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT EXISTS(SELECT 1 FROM task_generations g LEFT JOIN active_task_leases a
ON a.task_id = g.task_id AND a.attempt = g.attempt
WHERE g.task_id = ? AND g.state IN ('downloading','delivering') AND a.task_id IS NULL)
)SQL"};
    if (!select || !bind_text(select.get(), 1, task_id) ||
        sqlite3_step(select.get()) != SQLITE_ROW)
        return storage_error("unable to inspect active generation recovery material");
    return sqlite3_column_int(select.get(), 0) != 0;
}

Result<ActiveTaskLease> DurableStateStore::active_task_lease(const TaskGenerationKey& key) const {
    if (!is_valid_uuid(key.task_id) || key.attempt < 1) return state_error("generation is invalid");
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT a.batch_id, a.project_id, a.resource_type, a.resource_revision_id,
       a.max_attempts, a.lease_token, a.lease_expires_at, g.progress
FROM active_task_leases a JOIN task_generations g
ON a.task_id = g.task_id AND a.attempt = g.attempt
WHERE a.task_id = ? AND a.attempt = ? AND g.state IN ('accepted','downloading','delivering')
)SQL"};
    if (!select || !bind_text(select.get(), 1, key.task_id) ||
        sqlite3_bind_int64(select.get(), 2, key.attempt) != SQLITE_OK ||
        sqlite3_step(select.get()) != SQLITE_ROW)
        return state_error("no recoverable active lease exists; do not accept an active task");
    TaskAvailableMessage n;
    n.task_id = key.task_id;
    n.batch_id = column_text(select.get(), 0);
    n.project_id = column_text(select.get(), 1);
    n.resource_type = column_text(select.get(), 2) == "map" ? ResourceType::map : ResourceType::road_network;
    n.resource_revision_id = column_text(select.get(), 3);
    n.attempt = key.attempt;
    n.max_attempts = sqlite3_column_int64(select.get(), 4);
    return ActiveTaskLease{n, key, column_text(select.get(), 5),
        sqlite3_column_int64(select.get(), 6), sqlite3_column_int64(select.get(), 7)};
}

Result<void> DurableStateStore::persist_status_event(const StatusEventRecord& event) {
    if (!is_valid_uuid(event.event_id) || !is_valid_uuid(event.generation.task_id) ||
        event.generation.attempt < 1 || !is_reportable_state(event.state) ||
        event.progress < 0 || event.progress > 100 || event.payload_json.empty() ||
        event.lease_token.empty() || event.lease_token.size() > 8192U ||
        event.lease_expires_at <= 0 || event.local_delivery_evidence.empty() ||
        event.local_delivery_evidence.size() > 16384U ||
        event.disposition != StatusEventDisposition::pending ||
        !event.reconciliation_reason.empty() ||
        event.error_code.size() > 64 || event.error_message.size() > 512 ||
        (event.state == TaskExecutionState::delivered && event.progress != 100) ||
        (event.state == TaskExecutionState::failed && event.error_code.empty())) {
        return state_error("status event is invalid");
    }
    const auto digest = sha256_hex(event.payload_json);
    if (!digest) {
        return digest.error();
    }
    if (!event.request_sha256.empty() && event.request_sha256 != digest.value()) {
        return Error{ErrorDomain::storage, ErrorCode::integrity_failure, false,
                     "status request digest does not match its exact payload"};
    }

    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    if (!execute(impl_->database, "BEGIN IMMEDIATE")) {
        return storage_error("unable to begin status event transaction");
    }
    Statement replay{impl_->database, R"SQL(
SELECT task_id, attempt, state, progress, error_code, error_message, payload_json,
       request_sha256, lease_token, lease_expires_at, local_delivery_evidence,
       disposition, reconciliation_reason
FROM status_events WHERE event_id = ?
)SQL"};
    if (!replay || !bind_text(replay.get(), 1, event.event_id)) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("unable to query status event replay");
    }
    const int replay_step = sqlite3_step(replay.get());
    if (replay_step == SQLITE_ROW) {
        const auto text_at = [&replay](int column) {
            const auto* value = reinterpret_cast<const char*>(
                sqlite3_column_text(replay.get(), column));
            return value == nullptr ? std::string{} : std::string{value};
        };
        const bool identical = text_at(0) == event.generation.task_id &&
            sqlite3_column_int64(replay.get(), 1) == event.generation.attempt &&
            text_at(2) == state_name(event.state) &&
            sqlite3_column_int64(replay.get(), 3) == event.progress &&
            text_at(4) == event.error_code && text_at(5) == event.error_message &&
            text_at(6) == event.payload_json && text_at(7) == digest.value() &&
            text_at(8) == event.lease_token &&
            sqlite3_column_int64(replay.get(), 9) == event.lease_expires_at &&
            text_at(10) == event.local_delivery_evidence &&
            text_at(11) == disposition_name(event.disposition) &&
            text_at(12) == event.reconciliation_reason;
        execute(impl_->database, "ROLLBACK");
        return identical ? Result<void>{}
                         : Result<void>{state_error("event_id was reused with different content")};
    }
    if (replay_step != SQLITE_DONE) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("unable to query status event replay");
    }

    Statement generation{impl_->database, R"SQL(
SELECT state, progress_floor, progress FROM task_generations
WHERE task_id = ? AND attempt = ?
)SQL"};
    if (!generation || !bind_text(generation.get(), 1, event.generation.task_id) ||
        sqlite3_bind_int64(generation.get(), 2, event.generation.attempt) != SQLITE_OK ||
        sqlite3_step(generation.get()) != SQLITE_ROW) {
        execute(impl_->database, "ROLLBACK");
        return state_error("task generation does not exist");
    }
    const auto* current_text = reinterpret_cast<const char*>(
        sqlite3_column_text(generation.get(), 0));
    TaskExecutionState current{};
    if (current_text == nullptr || !parse_state(current_text, current)) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("stored task generation state is invalid");
    }
    const auto floor = sqlite3_column_int64(generation.get(), 1);
    const auto progress = sqlite3_column_int64(generation.get(), 2);
    if (event.progress < floor || event.progress < progress ||
        !valid_transition(current, event.state)) {
        execute(impl_->database, "ROLLBACK");
        return state_error("status event violates progress or transition rules");
    }
    Statement binding{impl_->database,
        "SELECT lease_token, lease_expires_at FROM active_task_leases WHERE task_id = ? AND attempt = ?"};
    if (!binding || !bind_text(binding.get(), 1, event.generation.task_id) ||
        sqlite3_bind_int64(binding.get(), 2, event.generation.attempt) != SQLITE_OK) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("unable to verify status active lease binding");
    }
    const int bound = sqlite3_step(binding.get());
    if (bound != SQLITE_ROW && bound != SQLITE_DONE) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("unable to read status active lease binding");
    }
    if (bound == SQLITE_ROW && (column_text(binding.get(), 0) != event.lease_token ||
        sqlite3_column_int64(binding.get(), 1) != event.lease_expires_at)) {
        execute(impl_->database, "ROLLBACK");
        return state_error("new status event differs from original active lease binding");
    }
    Statement insert{impl_->database, R"SQL(
INSERT INTO status_events(event_id, task_id, attempt, state, progress,
                          error_code, error_message, payload_json, acknowledged, created_at,
                          request_sha256, lease_token, lease_expires_at,
                          local_delivery_evidence, disposition, reconciliation_reason)
VALUES(?, ?, ?, ?, ?, ?, ?, ?, 0, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'),
       ?, ?, ?, ?, 'pending', NULL)
)SQL"};
    const auto state = state_name(event.state);
    if (!insert || !bind_text(insert.get(), 1, event.event_id) ||
        !bind_text(insert.get(), 2, event.generation.task_id) ||
        sqlite3_bind_int64(insert.get(), 3, event.generation.attempt) != SQLITE_OK ||
        !bind_text(insert.get(), 4, state) ||
        sqlite3_bind_int64(insert.get(), 5, event.progress) != SQLITE_OK ||
        !bind_text(insert.get(), 6, event.error_code) ||
        !bind_text(insert.get(), 7, event.error_message) ||
        !bind_text(insert.get(), 8, event.payload_json) ||
        !bind_text(insert.get(), 9, digest.value()) ||
        !bind_text(insert.get(), 10, event.lease_token) ||
        sqlite3_bind_int64(insert.get(), 11, event.lease_expires_at) != SQLITE_OK ||
        !bind_text(insert.get(), 12, event.local_delivery_evidence) ||
        sqlite3_step(insert.get()) != SQLITE_DONE) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("unable to persist status event");
    }
    Statement update{impl_->database, R"SQL(
UPDATE task_generations SET state = ?, progress = ?,
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
WHERE task_id = ? AND attempt = ?
)SQL"};
    if (!update || !bind_text(update.get(), 1, state) ||
        sqlite3_bind_int64(update.get(), 2, event.progress) != SQLITE_OK ||
        !bind_text(update.get(), 3, event.generation.task_id) ||
        sqlite3_bind_int64(update.get(), 4, event.generation.attempt) != SQLITE_OK ||
        sqlite3_step(update.get()) != SQLITE_DONE ||
        !execute(impl_->database, "COMMIT")) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("unable to update task generation state");
    }
    if (!impl_->harden()) {
        return storage_error("unable to apply private state database permissions");
    }
    return {};
}

Result<StatusEventRecord> DurableStateStore::status_event(
    std::string_view event_id) const {
    if (!is_valid_uuid(event_id)) {
        return state_error("event_id is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT event_id, task_id, attempt, state, progress, error_code, error_message,
       payload_json, request_sha256, lease_token, lease_expires_at,
       local_delivery_evidence, disposition, reconciliation_reason
FROM status_events WHERE event_id = ?
)SQL"};
    if (!select || !bind_text(select.get(), 1, event_id)) {
        return storage_error("unable to query status event");
    }
    if (sqlite3_step(select.get()) != SQLITE_ROW) {
        return state_error("status event does not exist");
    }
    return read_status_event_row(select.get());
}

Result<void> DurableStateStore::acknowledge_status_event(std::string_view event_id) {
    if (!is_valid_uuid(event_id)) {
        return state_error("event_id is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    Statement update{impl_->database, R"SQL(
UPDATE status_events
SET acknowledged = 1, disposition = 'acknowledged', lease_token = ''
WHERE event_id = ? AND disposition = 'pending'
)SQL"};
    if (!update || !bind_text(update.get(), 1, event_id) ||
        sqlite3_step(update.get()) != SQLITE_DONE) {
        return storage_error("unable to acknowledge status event");
    }
    if (sqlite3_changes(impl_->database) != 1) {
        return state_error("pending status event does not exist");
    }
    if (!impl_->harden()) {
        return storage_error("unable to apply private state database permissions");
    }
    return {};
}

Result<void> DurableStateStore::mark_status_event_reconciliation_required(
    std::string_view event_id,
    std::string_view reason) {
    if (!is_valid_uuid(event_id) || reason.empty() || reason.size() > 128U) {
        return state_error("reconciliation request is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    if (!execute(impl_->database, "BEGIN IMMEDIATE")) {
        return storage_error("unable to begin reconciliation transaction");
    }
    Statement update_event{impl_->database, R"SQL(
UPDATE status_events
SET disposition = 'reconciliation_required', reconciliation_reason = ?
WHERE event_id = ? AND disposition = 'pending'
  AND state IN ('delivered', 'failed')
)SQL"};
    if (!update_event || !bind_text(update_event.get(), 1, reason) ||
        !bind_text(update_event.get(), 2, event_id) ||
        sqlite3_step(update_event.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->database) != 1) {
        execute(impl_->database, "ROLLBACK");
        return state_error("terminal pending status event does not exist");
    }
    Statement update_generation{impl_->database, R"SQL(
UPDATE task_generations
SET state = 'reconciliation_required',
    updated_at = strftime('%Y-%m-%dT%H:%M:%fZ', 'now')
WHERE (task_id, attempt) = (
    SELECT task_id, attempt FROM status_events WHERE event_id = ?
)
)SQL"};
    if (!update_generation || !bind_text(update_generation.get(), 1, event_id) ||
        sqlite3_step(update_generation.get()) != SQLITE_DONE ||
        sqlite3_changes(impl_->database) != 1 ||
        !execute(impl_->database, "COMMIT")) {
        execute(impl_->database, "ROLLBACK");
        return storage_error("unable to mark task reconciliation required");
    }
    if (!impl_->harden()) {
        return storage_error("unable to apply private state database permissions");
    }
    return {};
}

Result<void> DurableStateStore::resolve_status_event_reconciliation(
    std::string_view event_id) {
    if (!is_valid_uuid(event_id)) {
        return state_error("event_id is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    if (!impl_->harden()) return storage_error("state paths are not private");
    Statement update{impl_->database, R"SQL(
UPDATE status_events
SET acknowledged = 1, disposition = 'resolved', lease_token = ''
WHERE event_id = ? AND disposition = 'reconciliation_required'
)SQL"};
    if (!update || !bind_text(update.get(), 1, event_id) ||
        sqlite3_step(update.get()) != SQLITE_DONE) {
        return storage_error("unable to resolve status event reconciliation");
    }
    if (sqlite3_changes(impl_->database) != 1) {
        return state_error("reconciliation-required status event does not exist");
    }
    if (!impl_->harden()) {
        return storage_error("unable to apply private state database permissions");
    }
    return {};
}

Result<std::vector<StatusEventRecord>> DurableStateStore::pending_status_events() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT e.event_id, e.task_id, e.attempt, e.state, e.progress,
       e.error_code, e.error_message, e.payload_json, e.request_sha256,
       e.lease_token, e.lease_expires_at, e.local_delivery_evidence,
       e.disposition, e.reconciliation_reason
FROM status_events e
JOIN task_generations g ON g.task_id = e.task_id AND g.attempt = e.attempt
WHERE e.disposition = 'pending'
  AND (e.state IN ('delivered', 'failed') OR g.state NOT IN ('expired', 'canceled'))
ORDER BY e.rowid
)SQL"};
    if (!select) {
        return storage_error("unable to query pending status events");
    }
    std::vector<StatusEventRecord> events;
    while (true) {
        const int step = sqlite3_step(select.get());
        if (step == SQLITE_DONE) {
            return events;
        }
        if (step != SQLITE_ROW) {
            return storage_error("unable to read pending status events");
        }
        const auto event = read_status_event_row(select.get());
        if (!event) {
            return event.error();
        }
        events.push_back(event.value());
    }
}

Result<std::vector<StatusEventRecord>>
DurableStateStore::reconciliation_required_events() const {
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT event_id, task_id, attempt, state, progress, error_code, error_message,
       payload_json, request_sha256, lease_token, lease_expires_at,
       local_delivery_evidence, disposition, reconciliation_reason
FROM status_events
WHERE disposition = 'reconciliation_required'
ORDER BY rowid
)SQL"};
    if (!select) {
        return storage_error("unable to query reconciliation-required events");
    }
    std::vector<StatusEventRecord> events;
    while (true) {
        const int step = sqlite3_step(select.get());
        if (step == SQLITE_DONE) {
            return events;
        }
        if (step != SQLITE_ROW) {
            return storage_error("unable to read reconciliation-required events");
        }
        const auto event = read_status_event_row(select.get());
        if (!event) {
            return event.error();
        }
        events.push_back(event.value());
    }
}

Result<bool> DurableStateStore::has_unconfirmed_terminal_event(
    std::string_view task_id) const {
    if (!is_valid_uuid(task_id)) {
        return state_error("task_id is invalid");
    }
    std::lock_guard<std::mutex> lock{impl_->mutex};
    Statement select{impl_->database, R"SQL(
SELECT EXISTS(
    SELECT 1 FROM status_events
    WHERE task_id = ?
      AND state IN ('delivered', 'failed')
      AND disposition IN ('pending', 'reconciliation_required')
)
)SQL"};
    if (!select || !bind_text(select.get(), 1, task_id) ||
        sqlite3_step(select.get()) != SQLITE_ROW) {
        return storage_error("unable to query terminal outbox state");
    }
    return sqlite3_column_int(select.get(), 0) != 0;
}

}  // namespace roc::vehicle
