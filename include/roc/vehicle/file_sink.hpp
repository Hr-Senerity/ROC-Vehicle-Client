#pragma once

#include "roc/vehicle/result.hpp"
#include "roc/vehicle/task_http_client.hpp"
#include "roc/vehicle/task_protocol.hpp"
#include "roc/vehicle/road_network.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

namespace roc::vehicle {

struct FileSinkOptions {
    std::string expected_coordinate_mode;
    std::size_t maximum_road_document_bytes{64U*1024U*1024U};
};

struct DeliveredArtifact {
    std::filesystem::path path;
    std::string sha256;
    std::uint64_t byte_size{0};
    std::string content_type;
    std::string task_id;
    ResourceType resource_type{ResourceType::map};
    std::string resource_revision_id;
    std::int64_t version{0};
    std::int64_t schema_version{0};
};

// A FileSink has three phases: write, seal, and commit. seal verifies response
// metadata, length, SHA-256, and road-network semantics and fsyncs the part
// file. commit rehashes, atomically installs it and syncs the parent directory. This split
// lets orchestration report `delivering` after seal but before commit.
class FileSink {
public:
    static Result<std::unique_ptr<FileSink>> create(
        std::filesystem::path artifact_root,
        ArtifactManifest manifest,
        FileSinkOptions options = {});

    FileSink(const FileSink&) = delete;
    FileSink& operator=(const FileSink&) = delete;
    FileSink(FileSink&&) = delete;
    FileSink& operator=(FileSink&&) = delete;
    ~FileSink();

    [[nodiscard]] Result<void> write(const std::uint8_t* data, std::size_t size);
    [[nodiscard]] Result<void> seal(const ArtifactHttpMetadata& metadata);
    [[nodiscard]] Result<DeliveredArtifact> commit();
    void abort() noexcept;

    [[nodiscard]] const std::filesystem::path& part_path() const noexcept;
    [[nodiscard]] const std::filesystem::path& final_path() const noexcept;
    [[nodiscard]] std::uint64_t bytes_written() const noexcept;

private:
    struct Impl;
    explicit FileSink(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

// Removes only direct children created with FileSink's `.roc-*.part` naming
// convention. It never recursively follows server-controlled paths.
[[nodiscard]] Result<std::size_t> recover_file_sink_parts(
    const std::filesystem::path& artifact_root);

// Revalidate an existing generated final against a trusted bound manifest,
// including road semantics, and resync before reuse. No network or .part creation.
[[nodiscard]] Result<DeliveredArtifact> inspect_file_artifact(
    const std::filesystem::path& artifact_root,
    const ArtifactManifest& manifest,FileSinkOptions options = {});

}  // namespace roc::vehicle
