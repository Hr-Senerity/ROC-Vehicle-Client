#pragma once
#include "roc/vehicle/config.hpp"
#include "roc/vehicle/task_protocol.hpp"
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace roc::vehicle::driver {
// Administrative input, not a Device Protocol field or a value inferred from an artifact.
struct OwningMapContext {
    std::string project_id, resource_revision_id, artifact_sha256;
    std::string map_id, map_revision_id, coordinate_mode;
};
struct Settings {
    ClientConfig client;
    std::filesystem::path token_file;
    std::vector<OwningMapContext> owning_maps;
    bool owning_maps_configured{false};
    bool remote{false};
    int retry_attempts{3};
    int retry_initial_ms{500};
    int retry_maximum_ms{5000};
    int offline_budget_ms{60000};
};
[[nodiscard]] Error local_error(const char* code, ErrorDomain domain = ErrorDomain::configuration);
[[nodiscard]] Result<Settings> load_settings(const std::filesystem::path& path, bool allow_remote);
// Read-only: no SQLite open, chmod, mkdir, lock, resolver or socket operation.
[[nodiscard]] Result<void> preflight(const Settings& settings);
[[nodiscard]] Result<std::string> read_token(const std::filesystem::path& path);
[[nodiscard]] Result<std::string> read_private_file(const std::filesystem::path& path, std::size_t limit);
[[nodiscard]] Result<std::vector<OwningMapContext>> read_owning_maps(
    const std::filesystem::path& path, std::string_view vehicle_id);
[[nodiscard]] Result<std::string> bind_owning_map(const Settings& settings, const ArtifactManifest& manifest,
                                                const TaskGenerationKey& key);
[[nodiscard]] Result<void> save_immutable(const std::filesystem::path& path, std::string_view bytes);
[[nodiscard]] std::string manifest_document(const ArtifactManifest& manifest);
[[nodiscard]] std::filesystem::path manifest_path(const Settings& settings, const TaskGenerationKey& key);

class RootLocks {
public:
    static Result<std::unique_ptr<RootLocks>> acquire(const Settings& settings);
    ~RootLocks();
    RootLocks(const RootLocks&) = delete;
    RootLocks& operator=(const RootLocks&) = delete;
private:
    RootLocks() = default;
    std::vector<int> descriptors_;
};
}
