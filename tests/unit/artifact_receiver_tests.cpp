#include "roc/vehicle/artifact_receiver.hpp"
#include "roc/vehicle/crypto.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "line " << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(expression) expect(static_cast<bool>(expression), #expression, __LINE__)

roc::vehicle::DeliveredArtifact make_artifact(
    const std::filesystem::path& root,
    std::string_view content = "controlled-receiver-data") {
    std::filesystem::create_directories(root);
    const auto path = root / "verified.bin";
    {
        std::ofstream output{path, std::ios::binary};
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
    }
    const auto digest = roc::vehicle::sha256_hex(content);
    EXPECT(digest);
    return {path,
            digest ? digest.value() : std::string(64, '0'),
            content.size(),
            "image/png",
            "123e4567-e89b-12d3-a456-426614174010",
            roc::vehicle::ResourceType::map,
            "123e4567-e89b-12d3-a456-426614174013",
            1};
}

struct TransactionState {
    std::string bytes;
    bool began{false};
    bool committed{false};
    bool aborted{false};
    bool fail_commit{false};
    roc::vehicle::CancellationSignal* cancel_after_write{nullptr};
};

class TestTransaction final : public roc::vehicle::ArtifactStreamTransaction {
public:
    explicit TestTransaction(TransactionState& state) : state_(state) {}

    roc::vehicle::Result<void> write(const std::uint8_t* data,
                                     std::size_t size) override {
        state_.bytes.append(reinterpret_cast<const char*>(data), size);
        if (state_.cancel_after_write != nullptr) {
            state_.cancel_after_write->cancel();
            state_.cancel_after_write = nullptr;
        }
        return {};
    }

    roc::vehicle::Result<void> commit() override {
        if (state_.fail_commit) {
            return roc::vehicle::Error{roc::vehicle::ErrorDomain::artifact,
                                       roc::vehicle::ErrorCode::io_failure, false,
                                       "test commit failed"};
        }
        state_.committed = true;
        return {};
    }

    void abort() noexcept override { state_.aborted = true; }

private:
    TransactionState& state_;
};

class TestReceiver final : public roc::vehicle::TransactionalStreamReceiver {
public:
    explicit TestReceiver(TransactionState& state) : state_(state) {}

    roc::vehicle::Result<std::unique_ptr<roc::vehicle::ArtifactStreamTransaction>>
    begin(const roc::vehicle::DeliveredArtifact& artifact) override {
        state_.began = artifact.task_id ==
            "123e4567-e89b-12d3-a456-426614174010";
        return std::unique_ptr<roc::vehicle::ArtifactStreamTransaction>{
            new TestTransaction{state_}};
    }

private:
    TransactionState& state_;
};

void test_verified_file_callback(const std::filesystem::path& root) {
    const auto artifact = make_artifact(root / "callback");
    bool called = false;
    const auto receipt = roc::vehicle::deliver_to_verified_file_callback(
        artifact, [&called](const roc::vehicle::DeliveredArtifact& delivered) {
            called = std::filesystem::is_regular_file(delivered.path) &&
                     delivered.version == 1;
            return roc::vehicle::Result<void>{};
        });
    EXPECT(receipt);
    EXPECT(called);
    if (receipt) {
        EXPECT(receipt.value().committed);
    }

    auto forged = artifact;
    forged.sha256 = std::string(64, '0');
    called = false;
    EXPECT(!roc::vehicle::deliver_to_verified_file_callback(
        forged, [&called](const roc::vehicle::DeliveredArtifact&) {
            called = true;
            return roc::vehicle::Result<void>{};
        }));
    EXPECT(!called);
}

void test_stream_commit(const std::filesystem::path& root) {
    constexpr std::string_view content = "controlled-receiver-data";
    const auto artifact = make_artifact(root / "stream", content);
    TransactionState state;
    TestReceiver receiver{state};
    const auto receipt = roc::vehicle::deliver_to_transactional_stream(
        artifact, receiver, 4);
    EXPECT(receipt);
    EXPECT(state.began);
    EXPECT(state.bytes == content);
    EXPECT(state.committed);
    EXPECT(!state.aborted);
    if (receipt) {
        EXPECT(receipt.value().committed);
    }
}

void test_cancel_aborts(const std::filesystem::path& root) {
    const auto artifact = make_artifact(root / "cancel");
    roc::vehicle::CancellationSignal cancellation;
    TransactionState state;
    state.cancel_after_write = &cancellation;
    TestReceiver receiver{state};
    const auto receipt = roc::vehicle::deliver_to_transactional_stream(
        artifact, receiver, 4, &cancellation);
    EXPECT(!receipt);
    EXPECT(state.began);
    EXPECT(!state.committed);
    EXPECT(state.aborted);
}

void test_commit_failure_aborts(const std::filesystem::path& root) {
    const auto artifact = make_artifact(root / "commit-failure");
    TransactionState state;
    state.fail_commit = true;
    TestReceiver receiver{state};
    const auto receipt = roc::vehicle::deliver_to_transactional_stream(
        artifact, receiver, 8);
    EXPECT(!receipt);
    EXPECT(!state.committed);
    EXPECT(state.aborted);
}

}  // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() /
                      "roc-vehicle-artifact-receiver-test";
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    test_verified_file_callback(root);
    test_stream_commit(root);
    test_cancel_aborts(root);
    test_commit_failure_aborts(root);
    std::filesystem::remove_all(root, ignored);
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
