#pragma once
#include "roc/vehicle/task_http_client.hpp"
#include <filesystem>
#include <functional>
#include <memory>

namespace roc::vehicle {
struct DeliveredMapPackage {
    std::filesystem::path directory;
    ArtifactManifest manifest;
    std::string descriptor_sha256;
};
// Linux durable immutable-directory publication. The object is worker-confined.
// All files must seal, then the package seals, then a single package commit.
// No per-file delivered result exists. Non-Linux platforms fail closed.
class PackageSink {
public:
    static Result<std::unique_ptr<PackageSink>> create(
        std::filesystem::path dedicated_root, ArtifactManifest manifest,
        ArtifactLimits limits = {});
    PackageSink(const PackageSink&) = delete;
    PackageSink& operator=(const PackageSink&) = delete;
    ~PackageSink();
    [[nodiscard]] Result<void> write(std::string_view file_id, const std::uint8_t* data,
                                     std::size_t size);
    [[nodiscard]] Result<void> seal_file(std::string_view file_id, const ArtifactHttpMetadata& metadata);
    [[nodiscard]] Result<void> seal();
    [[nodiscard]] Result<DeliveredMapPackage> commit();
    void abort() noexcept;
    [[nodiscard]] const std::filesystem::path& staging_path() const noexcept;
private:
    struct Impl;
    explicit PackageSink(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
// Re-hash and verify an already published package after crash/restart. Never accepts .part.
[[nodiscard]] Result<DeliveredMapPackage> inspect_map_package(
    const std::filesystem::path& dedicated_root, const ArtifactManifest& manifest);
[[nodiscard]] Result<void> verify_map_package(const DeliveredMapPackage& package);
// Explicit startup cleanup only; never run concurrently with active sinks in this root.
// Deletes only recognized staging directories and direct generated regular files.
[[nodiscard]] Result<std::size_t> recover_package_staging(const std::filesystem::path& dedicated_root);
[[nodiscard]] Result<std::string> make_map_package_evidence(
    const DeliveredMapPackage& package, const TaskGenerationKey& generation);
using PackageChunkHandler = std::function<Result<void>(std::string_view file_id,
    const std::uint8_t*, std::size_t)>;
// Transactional receiver helper: stream hash verification finishes before caller commit.
[[nodiscard]] Result<void> stream_map_package(const DeliveredMapPackage& package,
    const PackageChunkHandler& handler, std::size_t chunk_size = 64U * 1024U,
    const CancellationSignal* cancellation = nullptr);
}
