#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/file_sink.hpp"
#include "roc/vehicle/state_store.hpp"
#include "roc/vehicle/task_protocol.hpp"

#include <sqlite3.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#ifndef _WIN32
#include <csignal>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

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
constexpr auto kLease = "opaque-crash-test-lease";
constexpr std::int64_t kLeaseExpiry = 4102444800;

#ifndef _WIN32
[[noreturn]] void strong_kill() {
    ::kill(::getpid(), SIGKILL);
    std::_Exit(127);
}

bool child_was_killed(pid_t child) {
    int status = 0;
    return ::waitpid(child, &status, 0) == child &&
           WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
}

bool child_exited(pid_t child, int expected) {
    int status = 0;
    return ::waitpid(child, &status, 0) == child &&
           WIFEXITED(status) && WEXITSTATUS(status) == expected;
}

roc::vehicle::StatusEventRecord make_event(
    std::string event_id,
    roc::vehicle::TaskExecutionState state,
    std::int64_t progress) {
    roc::vehicle::StatusEventRecord event{
        std::move(event_id), {kTaskId, 1}, state, progress, {}, {}, {}, {},
        kLease, kLeaseExpiry,
        R"JSON({"receiver":"file","fsync":true,"crash_test":true})JSON"};
    const auto payload = roc::vehicle::make_status_request(event);
    if (!payload) {
        return {};
    }
    event.payload_json = payload.value();
    return event;
}

bool persist_terminal_outbox(roc::vehicle::DurableStateStore& store) {
    const auto registered = store.register_task_generation({kTaskId, 1}, 20);
    auto downloading = make_event(
        "123e4567-e89b-12d3-a456-426614174041",
        roc::vehicle::TaskExecutionState::downloading, 20);
    auto delivering = make_event(
        "123e4567-e89b-12d3-a456-426614174042",
        roc::vehicle::TaskExecutionState::delivering, 80);
    auto delivered = make_event(
        "123e4567-e89b-12d3-a456-426614174043",
        roc::vehicle::TaskExecutionState::delivered, 100);
    return registered && !downloading.event_id.empty() &&
           store.persist_status_event(downloading) &&
           store.acknowledge_status_event(downloading.event_id) &&
           store.persist_status_event(delivering) &&
           store.acknowledge_status_event(delivering.event_id) &&
           store.persist_status_event(delivered);
}

void test_sequence_reservation_survives_sigkill(
    const std::filesystem::path& root) {
    const auto database = root / "state" / "state.sqlite3";
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        if (!opened) {
            std::_Exit(20);
        }
        const auto sequence = opened.value()->allocate_sequence();
        if (!sequence || sequence.value() != 1) {
            std::_Exit(21);
        }
        strong_kill();
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(reopened);
    if (reopened) {
        const auto sequence = reopened.value()->allocate_sequence();
        EXPECT(sequence);
        if (sequence) {
            EXPECT(sequence.value() == 5);
        }
    }
}

void test_uncommitted_wal_transaction_rolls_back_after_sigkill(
    const std::filesystem::path& root) {
    const auto database = root / "state" / "state.sqlite3";
    {
        auto initialized = roc::vehicle::DurableStateStore::open(database, 4);
        EXPECT(initialized);
    }
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        sqlite3* raw = nullptr;
        if (sqlite3_open(database.c_str(), &raw) != SQLITE_OK || raw == nullptr) {
            std::_Exit(30);
        }
        const int result = sqlite3_exec(
            raw,
            "BEGIN IMMEDIATE; UPDATE sequence_state SET reserved_through = 999 "
            "WHERE singleton = 1;",
            nullptr, nullptr, nullptr);
        if (result != SQLITE_OK) {
            std::_Exit(31);
        }
        strong_kill();
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(reopened);
    if (reopened) {
        const auto sequence = reopened.value()->allocate_sequence();
        EXPECT(sequence);
        if (sequence) {
            EXPECT(sequence.value() == 1);
        }
    }
}

void test_terminal_outbox_survives_sigkill(
    const std::filesystem::path& root) {
    const auto database = root / "state" / "state.sqlite3";
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        if (!opened || !persist_terminal_outbox(*opened.value())) {
            std::_Exit(40);
        }
        strong_kill();
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(reopened);
    if (reopened) {
        const auto pending = reopened.value()->pending_status_events();
        EXPECT(pending);
        if (pending) {
            EXPECT(pending.value().size() == 1);
            if (pending.value().size() == 1) {
                const auto& event = pending.value().front();
                EXPECT(event.state == roc::vehicle::TaskExecutionState::delivered);
                EXPECT(event.event_id ==
                       "123e4567-e89b-12d3-a456-426614174043");
                EXPECT(event.lease_token == kLease);
                EXPECT(event.request_sha256.size() == 64);
                EXPECT(!event.local_delivery_evidence.empty());
            }
        }
        const auto blocked =
            reopened.value()->has_unconfirmed_terminal_event(kTaskId);
        EXPECT(blocked);
        if (blocked) {
            EXPECT(blocked.value());
        }
    }
}

void expect_terminal_event_released(const std::filesystem::path& database,
                                    roc::vehicle::StatusEventDisposition expected) {
    auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(reopened);
    if (!reopened) {
        return;
    }
    const auto event = reopened.value()->status_event(
        "123e4567-e89b-12d3-a456-426614174043");
    EXPECT(event);
    if (event) {
        EXPECT(event.value().disposition == expected);
        EXPECT(event.value().lease_token.empty());
        EXPECT(!event.value().local_delivery_evidence.empty());
    }
    const auto pending = reopened.value()->pending_status_events();
    EXPECT(pending);
    if (pending) {
        EXPECT(pending.value().empty());
    }
    const auto blocked = reopened.value()->has_unconfirmed_terminal_event(kTaskId);
    EXPECT(blocked);
    if (blocked) {
        EXPECT(!blocked.value());
    }
}

void test_terminal_ack_survives_immediate_sigkill(
    const std::filesystem::path& root) {
    const auto database = root / "state" / "state.sqlite3";
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        if (!opened || !persist_terminal_outbox(*opened.value()) ||
            !opened.value()->acknowledge_status_event(
                "123e4567-e89b-12d3-a456-426614174043")) {
            std::_Exit(45);
        }
        strong_kill();
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    expect_terminal_event_released(
        database, roc::vehicle::StatusEventDisposition::acknowledged);
}

void test_reconciliation_resolution_survives_immediate_sigkill(
    const std::filesystem::path& root) {
    const auto database = root / "state" / "state.sqlite3";
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        constexpr auto event_id =
            "123e4567-e89b-12d3-a456-426614174043";
        if (!opened || !persist_terminal_outbox(*opened.value()) ||
            !opened.value()->mark_status_event_reconciliation_required(
                event_id, "crash_test") ||
            !opened.value()->resolve_status_event_reconciliation(event_id)) {
            std::_Exit(46);
        }
        strong_kill();
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    expect_terminal_event_released(
        database, roc::vehicle::StatusEventDisposition::resolved);
}

roc::vehicle::ArtifactManifest crash_manifest(std::string_view content) {
    const auto digest = roc::vehicle::sha256_hex(content);
    return {kTaskId,
            "123e4567-e89b-12d3-a456-426614174011",
            "123e4567-e89b-12d3-a456-426614174012",
            roc::vehicle::ResourceType::map,
            "123e4567-e89b-12d3-a456-426614174013",
            "123e4567-e89b-12d3-a456-426614174014",
            "image/png",
            content.size(),
            digest ? digest.value() : std::string(64, '0'),
            1,
            0,
            std::string{"/api/device/tasks/"} + kTaskId + "/artifact"};
}

void test_part_file_recovery_after_sigkill(const std::filesystem::path& root) {
    const auto artifacts = root / "artifacts";
    const std::string content = "artifact-data";
    const auto manifest = crash_manifest(content);
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto sink = roc::vehicle::FileSink::create(artifacts, manifest);
        if (!sink || !sink.value()->write(
                reinterpret_cast<const std::uint8_t*>(content.data()), 4)) {
            std::_Exit(50);
        }
        strong_kill();
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    std::size_t parts = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(artifacts, error)) {
        if (entry.path().filename().string().find(".roc-") == 0 &&
            entry.path().extension() == ".part") {
            ++parts;
        }
    }
    EXPECT(!error);
    EXPECT(parts == 1);
    const auto recovered = roc::vehicle::recover_file_sink_parts(artifacts);
    EXPECT(recovered);
    if (recovered) {
        EXPECT(recovered.value() == 1);
    }
    EXPECT(!std::filesystem::exists(
        artifacts / ("map-v1-" + manifest.sha256.substr(0, 12) + ".png")));
}

void test_database_write_limit_rolls_back(const std::filesystem::path& root) {
    const auto database = root / "state" / "state.sqlite3";
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto opened = roc::vehicle::DurableStateStore::open(database, 4);
        if (!opened ||
            !opened.value()->register_task_generation({kTaskId, 1}, 20)) {
            std::_Exit(60);
        }
        ::signal(SIGXFSZ, SIG_IGN);
        const struct rlimit limit {1, 1};
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::_Exit(61);
        }
        auto downloading = make_event(
            "123e4567-e89b-12d3-a456-426614174044",
            roc::vehicle::TaskExecutionState::downloading, 20);
        const auto persisted = opened.value()->persist_status_event(downloading);
        std::_Exit(persisted ? 62 : 0);
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_exited(child, 0));
    auto reopened = roc::vehicle::DurableStateStore::open(database, 4);
    EXPECT(reopened);
    if (reopened) {
        const auto generation = reopened.value()->task_generation({kTaskId, 1});
        EXPECT(generation);
        if (generation) {
            EXPECT(generation.value().state ==
                   roc::vehicle::TaskExecutionState::accepted);
            EXPECT(generation.value().progress == 20);
        }
        const auto pending = reopened.value()->pending_status_events();
        EXPECT(pending);
        if (pending) {
            EXPECT(pending.value().empty());
        }
    }
}

void test_artifact_write_limit_cleans_part(const std::filesystem::path& root) {
    const auto artifacts = root / "artifacts";
    const std::string content = "artifact-data";
    const auto manifest = crash_manifest(content);
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto sink = roc::vehicle::FileSink::create(artifacts, manifest);
        if (!sink) {
            std::_Exit(70);
        }
        const auto part = sink.value()->part_path();
        const auto final = sink.value()->final_path();
        ::signal(SIGXFSZ, SIG_IGN);
        const struct rlimit limit {1, 1};
        if (::setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            std::_Exit(71);
        }
        const auto written = sink.value()->write(
            reinterpret_cast<const std::uint8_t*>(content.data()),
            content.size());
        sink.value().reset();
        const bool clean = !std::filesystem::exists(part) &&
                           !std::filesystem::exists(final);
        std::_Exit(!written && clean ? 0 : 72);
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_exited(child, 0));
    const auto recovered = roc::vehicle::recover_file_sink_parts(artifacts);
    EXPECT(recovered);
    if (recovered) {
        EXPECT(recovered.value() == 0);
    }
}
#endif

}  // namespace

int main() {
#ifndef _WIN32
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-fault-recovery-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    test_sequence_reservation_survives_sigkill(root / "sequence");
    test_uncommitted_wal_transaction_rolls_back_after_sigkill(root / "wal");
    test_terminal_outbox_survives_sigkill(root / "outbox");
    test_terminal_ack_survives_immediate_sigkill(root / "ack");
    test_reconciliation_resolution_survives_immediate_sigkill(
        root / "resolve");
    test_part_file_recovery_after_sigkill(root / "part");
    test_database_write_limit_rolls_back(root / "database-limit");
    test_artifact_write_limit_cleans_part(root / "artifact-limit");
    std::filesystem::remove_all(root, ignored);
#endif
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
