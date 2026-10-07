#include "roc/vehicle/crypto.hpp"
#include "roc/vehicle/file_sink.hpp"
#include "../road_test_support.hpp"

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace {

enum class FaultAction : int { none, fail, kill_process };

std::atomic<int> fsync_action{static_cast<int>(FaultAction::none)};
std::atomic<int> rename_action{static_cast<int>(FaultAction::none)};
std::atomic<bool> write_failure{false};
bool road_mode=false;
int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: "
                  << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

[[noreturn]] void strong_kill() {
    ::kill(::getpid(), SIGKILL);
    std::_Exit(127);
}

bool child_was_killed(pid_t child) {
    int status = 0;
    return ::waitpid(child, &status, 0) == child && WIFSIGNALED(status) &&
           WTERMSIG(status) == SIGKILL;
}

roc::vehicle::ArtifactManifest manifest_for(std::string_view content) {
    if (road_mode) return road_test::manifest(content);
    const auto digest = roc::vehicle::sha256_hex(content);
    return {"123e4567-e89b-12d3-a456-426614174010",
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
            "/api/device/tasks/123e4567-e89b-12d3-a456-426614174010/artifact"};
}
std::string content_for() { return road_mode?road_test::fixture().to_string():"artifact-data"; }
std::filesystem::path final_for(const std::filesystem::path& root,const roc::vehicle::ArtifactManifest& manifest) {
    return root/(std::string{road_mode?"road-network-v":"map-v"}+std::to_string(manifest.version)+"-"+
        manifest.sha256.substr(0,12)+(road_mode?".json":".png"));
}

roc::vehicle::Result<std::unique_ptr<roc::vehicle::FileSink>>
create_written_sink(const std::filesystem::path& root,
                    const roc::vehicle::ArtifactManifest& manifest,
                    std::string_view content) {
    auto sink = roc::vehicle::FileSink::create(root, manifest);
    if (!sink) {
        return sink.error();
    }
    const auto written = sink.value()->write(
        reinterpret_cast<const std::uint8_t*>(content.data()), content.size());
    if (!written) {
        return written.error();
    }
    return sink;
}

roc::vehicle::Result<void> seal_sink(
    roc::vehicle::FileSink& sink,
    const roc::vehicle::ArtifactManifest& manifest) {
    return sink.seal({manifest.content_type, manifest.sha256,
                      manifest.byte_size, manifest.byte_size});
}

std::size_t part_count(const std::filesystem::path& root) {
    std::size_t count = 0;
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.path().filename().string().find(".roc-") == 0 &&
            entry.path().extension() == ".part") {
            ++count;
        }
    }
    EXPECT(!error);
    return count;
}

void test_file_fsync_failure_removes_part(const std::filesystem::path& root) {
    const auto content = content_for();
    const auto manifest = manifest_for(content);
    auto sink = create_written_sink(root, manifest, content);
    EXPECT(sink);
    if (!sink) {
        return;
    }
    fsync_action = static_cast<int>(FaultAction::fail);
    EXPECT(!seal_sink(*sink.value(), manifest));
    const auto final = sink.value()->final_path();
    sink.value().reset();
    EXPECT(part_count(root) == 0);
    EXPECT(!std::filesystem::exists(final));
}

void test_rename_failure_removes_part(const std::filesystem::path& root) {
    const auto content = content_for();
    const auto manifest = manifest_for(content);
    auto sink = create_written_sink(root, manifest, content);
    EXPECT(sink);
    if (!sink) {
        return;
    }
    EXPECT(seal_sink(*sink.value(), manifest));
    rename_action = static_cast<int>(FaultAction::fail);
    EXPECT(!sink.value()->commit());
    const auto final = sink.value()->final_path();
    sink.value().reset();
    EXPECT(part_count(root) == 0);
    EXPECT(!std::filesystem::exists(final));
}

void expect_retry_uses_verified_final(const std::filesystem::path& root) {
    const auto content = content_for();
    const auto manifest = manifest_for(content);
    auto retry = create_written_sink(root, manifest, content);
    EXPECT(retry);
    if (!retry) {
        return;
    }
    EXPECT(seal_sink(*retry.value(), manifest));
    const auto delivered = retry.value()->commit();
    EXPECT(delivered);
    if (delivered) {
        EXPECT(delivered.value().sha256 == manifest.sha256);
        EXPECT(delivered.value().byte_size == manifest.byte_size);
    }
    EXPECT(part_count(root) == 0);
}

void test_directory_fsync_failure_is_retryable(
    const std::filesystem::path& root) {
    const auto content = content_for();
    const auto manifest = manifest_for(content);
    auto sink = create_written_sink(root, manifest, content);
    EXPECT(sink);
    if (!sink) {
        return;
    }
    EXPECT(seal_sink(*sink.value(), manifest));
    fsync_action = static_cast<int>(FaultAction::fail);
    EXPECT(!sink.value()->commit());
    const auto final = sink.value()->final_path();
    EXPECT(sink.value()->commit()); // Retry the same sink after publication/sync ambiguity.
    sink.value().reset();
    EXPECT(std::filesystem::exists(final));
    EXPECT(part_count(root) == 0);
    expect_retry_uses_verified_final(root);
}

void test_sigkill_before_rename_recovers_part(
    const std::filesystem::path& root) {
    const auto content = content_for();
    const auto manifest = manifest_for(content);
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto sink = create_written_sink(root, manifest, content);
        if (!sink || !seal_sink(*sink.value(), manifest)) {
            std::_Exit(20);
        }
        rename_action = static_cast<int>(FaultAction::kill_process);
        static_cast<void>(sink.value()->commit());
        std::_Exit(21);
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    EXPECT(part_count(root) == 1);
    EXPECT(!std::filesystem::exists(
        final_for(root,manifest)));
    const auto recovered = roc::vehicle::recover_file_sink_parts(root);
    EXPECT(recovered);
    if (recovered) {
        EXPECT(recovered.value() == 1);
    }
}

void test_sigkill_after_rename_reuses_final(
    const std::filesystem::path& root) {
    const auto content = content_for();
    const auto manifest = manifest_for(content);
    const pid_t child = ::fork();
    EXPECT(child >= 0);
    if (child == 0) {
        auto sink = create_written_sink(root, manifest, content);
        if (!sink || !seal_sink(*sink.value(), manifest)) {
            std::_Exit(30);
        }
        fsync_action = static_cast<int>(FaultAction::kill_process);
        static_cast<void>(sink.value()->commit());
        std::_Exit(31);
    }
    if (child < 0) {
        return;
    }
    EXPECT(child_was_killed(child));
    EXPECT(part_count(root) == 0);
    EXPECT(std::filesystem::exists(
        final_for(root,manifest)));
    expect_retry_uses_verified_final(root);
    EXPECT(roc::vehicle::inspect_file_artifact(root,manifest));
}

void test_write_capacity_failure(const std::filesystem::path& root) {
    const auto content=content_for();const auto manifest=manifest_for(content);
    auto sink=roc::vehicle::FileSink::create(root,manifest);EXPECT(sink);if (!sink) return;
    write_failure=true;
    EXPECT(!sink.value()->write(reinterpret_cast<const std::uint8_t*>(content.data()),content.size()));
    EXPECT(!seal_sink(*sink.value(),manifest));const auto final=sink.value()->final_path();
    sink.value().reset();EXPECT(!std::filesystem::exists(final));EXPECT(part_count(root)==0);
}

}  // namespace

extern "C" int __real_fsync(int descriptor);
extern "C" int __real_rename(const char* old_path, const char* new_path);
extern "C" ssize_t __real_write(int descriptor,const void* data,std::size_t size);
extern "C" ssize_t __wrap_write(int descriptor,const void* data,std::size_t size) {
    if (write_failure.exchange(false)) { errno=ENOSPC;return -1; }
    return __real_write(descriptor,data,size);
}

extern "C" int __wrap_fsync(int descriptor) {
    const auto action = static_cast<FaultAction>(
        fsync_action.exchange(static_cast<int>(FaultAction::none)));
    if (action == FaultAction::kill_process) {
        strong_kill();
    }
    if (action == FaultAction::fail) {
        errno = EIO;
        return -1;
    }
    return __real_fsync(descriptor);
}

extern "C" int __wrap_rename(const char* old_path, const char* new_path) {
    const auto action = static_cast<FaultAction>(
        rename_action.exchange(static_cast<int>(FaultAction::none)));
    if (action == FaultAction::kill_process) {
        strong_kill();
    }
    if (action == FaultAction::fail) {
        errno = EIO;
        return -1;
    }
    return __real_rename(old_path, new_path);
}

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-file-sink-fault-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    for (const bool road:{false,true}) {
        road_mode=road;const auto branch=root/(road?"road-v2":"legacy-map");
        test_file_fsync_failure_removes_part(branch / "file-fsync");
        test_rename_failure_removes_part(branch / "rename");
        test_directory_fsync_failure_is_retryable(branch / "directory-fsync");
        test_sigkill_before_rename_recovers_part(branch / "kill-before-rename");
        test_sigkill_after_rename_reuses_final(branch / "kill-after-rename");
        test_write_capacity_failure(branch / "capacity");
    }
    std::filesystem::remove_all(root, ignored);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
